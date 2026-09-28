#!/usr/bin/env python3
"""Mock LLM server for Snowglobe proxy integration tests (stdlib only).

Serves OpenAI Chat Completions (any path containing "chat/completions") and
Anthropic Messages (any path ending in "/messages"), streaming and
non-streaming, driven by a JSON scenario script: a FIFO queue of responses.
When the queue is exhausted every further POST gets a loud 500
{"error":"scenario exhausted"} (never a silent empty 200).

Scenario file format:
  {"responses": [
    {"status": 200, "stream": false, "body": {"id": ..., ...}},
    {"status": 200, "stream": true, "chunks": ["data: {...}\\n\\n", ...],
     "end": "data: [DONE]"},
    {"status": 429, "stream": false, "body": {"error": {...}}},
    {"status": 200, "stream": false, "gen_bytes": 52428800},
    {"status": 200, "stream": true, "chunks_gen": {"count": 200,
     "bytes_each": 262144}}
  ]}
`body` is sent verbatim (tool calls included). `gen_bytes` / `chunks_gen`
emit deterministic repeating-pattern bytes so 50 MB bodies need no 50 MB
file. `content_type` overrides the default (application/json, or
text/event-stream for streams).

Flags:
  --port N (0 = ephemeral; the chosen port is printed as "READY <port>"),
  --scenario FILE, --headers-log FILE (one JSON object per request:
  {seq, method, path, headers (values verbatim), body_sha256, body_bytes}),
  --sent-dir DIR (writes req-<seq>.bin / res-<seq>.bin for byte-exactness
  asserts), --chunk-delay-ms (default sleep between stream chunks),
  --delay-last-chunk-ms (default extra sleep before the terminating
  zero-chunk). A single response can override both with "chunk_delay_ms" /
  "delay_last_ms" (used by the latency test: one delayed stream among
  quick ones).

GET /test-data returns a fixed JSON document (the toy agent's http_get
target). All other GETs 404. Received Authorization / x-api-key values are
logged VERBATIM here (this log never enters a trace); the proxy's stored
copy is what must read REDACTED.
"""
import argparse
import hashlib
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PATTERN = b"0123456789abcdef"


def gen_bytes(n, seed=0):
    out = bytearray()
    while len(out) < n:
        out += PATTERN
    return bytes(out[:n])


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockLLM/1B"

    def log_message(self, *a):
        pass

    def _send(self, status, body, ctype):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(data)

    def _send_chunked_head(self, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

    def _write_chunk(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.wfile.write(b"%x\r\n" % len(data))
        self.wfile.write(data)
        self.wfile.write(b"\r\n")
        self.wfile.flush()

    def do_GET(self):
        if self.path == "/test-data":
            self._send(200, {"msg": "mock-data-ok", "n": 3}, "application/json")
        else:
            self._send(404, {"error": "unknown mock route"}, "application/json")

    def do_POST(self):
        srv = self.server
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        with srv.lock:
            seq = srv.counter
            srv.counter += 1
        rec = {
            "seq": seq,
            "method": "POST",
            "path": self.path,
            "headers": {k.lower(): v for k, v in self.headers.items()},
            "body_sha256": hashlib.sha256(body).hexdigest(),
            "body_bytes": len(body),
        }
        if srv.sent_dir:
            with open(os.path.join(srv.sent_dir, "req-%04d.bin" % seq), "wb") as f:
                f.write(body)
        if srv.headers_log:
            with srv.log_lock:
                with open(srv.headers_log, "a") as f:
                    f.write(json.dumps(rec) + "\n")
        is_chat = "chat/completions" in self.path
        is_msg = self.path.rstrip("/").endswith("/messages")
        if not (is_chat or is_msg):
            self._send(404, {"error": "unknown mock route"}, "application/json")
            return
        with srv.lock:
            spec = srv.responses.pop(0) if srv.responses else None
        if spec is None:
            self._send(500, {"error": "scenario exhausted"}, "application/json")
            return
        status = spec.get("status", 200)
        if not spec.get("stream", False):
            if "gen_bytes" in spec:
                payload = gen_bytes(spec["gen_bytes"])
                ctype = spec.get("content_type", "application/octet-stream")
            else:
                payload = json.dumps(spec.get("body", {})).encode()
                ctype = spec.get("content_type", "application/json")
            if srv.sent_dir:
                with open(os.path.join(srv.sent_dir, "res-%04d.bin" % seq), "wb") as f:
                    f.write(payload)
            self._send(status, payload, ctype)
            return
        # Streaming: chunked SSE frames, per-chunk delays.
        ctype = spec.get("content_type", "text/event-stream")
        if "chunks_gen" in spec:
            g = spec["chunks_gen"]
            n, each = g["count"], g["bytes_each"]
            chunks = [b"data: " + gen_bytes(each, i) + b"\n\n" for i in range(n)]
            end = spec.get("end", "data: [DONE]").encode()
        else:
            chunks = [c.encode() if isinstance(c, str) else c
                      for c in spec.get("chunks", [])]
            end = spec.get("end", "data: [DONE]")
            end = end.encode() if isinstance(end, str) else end
        if status != 200:
            self._send(status, b'{"error":"mock scripted error"}',
                       "application/json")
            return
        self._send_chunked_head(ctype)
        sent = bytearray()
        cd = spec.get("chunk_delay_ms", srv.chunk_delay_ms)
        ld = spec.get("delay_last_ms", srv.delay_last_ms)
        try:
            for c in chunks:
                self._write_chunk(c)
                sent += c
                if cd:
                    time.sleep(cd / 1000.0)
            if ld:
                time.sleep(ld / 1000.0)
            self._write_chunk(end)
            sent += end
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        if srv.sent_dir:
            with open(os.path.join(srv.sent_dir, "res-%04d.bin" % seq), "wb") as f:
                f.write(bytes(sent))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--scenario", default="")
    ap.add_argument("--headers-log", default="")
    ap.add_argument("--sent-dir", default="")
    ap.add_argument("--chunk-delay-ms", type=int, default=0)
    ap.add_argument("--delay-last-chunk-ms", type=int, default=0)
    a = ap.parse_args()
    responses = []
    if a.scenario:
        with open(a.scenario) as f:
            responses = json.load(f).get("responses", [])
    if a.sent_dir:
        os.makedirs(a.sent_dir, exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    srv.allow_reuse_address = True
    srv.responses = responses
    srv.lock = threading.Lock()
    srv.log_lock = threading.Lock()
    srv.counter = 0
    srv.headers_log = a.headers_log
    srv.sent_dir = a.sent_dir
    srv.chunk_delay_ms = a.chunk_delay_ms
    srv.delay_last_ms = a.delay_last_chunk_ms
    print("READY %d" % srv.server_address[1], flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

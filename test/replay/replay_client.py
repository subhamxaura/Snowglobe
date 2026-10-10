#!/usr/bin/env python3
"""Traced helper: re-post recorded request envelopes at a replay proxy.

Reads <run>/llm/*.req.json in id order and POSTs each body verbatim to
the matching injected base URL (OPENAI_/ANTHROPIC_/GEMINI_*_BASE_URL +
the envelope path remainder), then exits with --exit-code. Used to replay
runs whose original agent binary is unavailable (e.g. the claude-code-1
error fixture): the LLM layer re-executes byte-for-byte while the exit
code emulates the original agent's failure.

Runs UNDER `snowglobe run` or `snowglobe replay` (needs injected base
URLs). NOT a CTest test itself.
Usage: replay_client.py --run DIR --results FILE [--exit-code N]
results: [{id, status, body_sha256, body_bytes}] in record order.
Non-2xx are recorded, not raised. Exits with --exit-code (default 0).
"""
import argparse
import glob
import hashlib
import http.client
import json
import os
import sys
import urllib.parse


def base_for(path):
    if path == "/openai" or path.startswith("/openai/"):
        return os.environ.get("OPENAI_BASE_URL"), path[7:] or "/"
    if path == "/anthropic" or path.startswith("/anthropic/"):
        return os.environ.get("ANTHROPIC_BASE_URL"), path[10:] or "/"
    if path == "/gemini" or path.startswith("/gemini/"):
        return os.environ.get("GEMINI_BASE_URL"), path[7:] or "/"
    return None, None


def one(base, remainder, env):
    u = urllib.parse.urlparse(base.rstrip("/") + remainder)
    data = env["raw_body"]
    conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=120)
    headers = {"Content-Type": env["headers"].get("Content-Type", "application/json")}
    conn.request(env["method"], u.path or "/", body=data, headers=headers)
    resp = conn.getresponse()
    chunks = []
    while True:
        b = resp.read(65536)
        if not b:
            break
        chunks.append(b)
    body = b"".join(chunks)
    out = {"status": resp.status,
           "body_sha256": hashlib.sha256(body).hexdigest(),
           "body_bytes": len(body)}
    conn.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--results", required=True)
    ap.add_argument("--exit-code", type=int, default=0)
    a = ap.parse_args()
    reqs = sorted(glob.glob(os.path.join(a.run, "llm", "*.req.json")))
    if not reqs:
        print("replay_client: no request envelopes in %s" % a.run,
              file=sys.stderr)
        return 2
    res = []
    for i, p in enumerate(reqs):
        with open(p, "rb") as f:
            raw = f.read()
        env = json.loads(raw.decode())
        base, remainder = base_for(env.get("path", ""))
        if not base:
            print("replay_client: no base URL for %s" % env.get("path"),
                  file=sys.stderr)
            return 2
        body = env.get("body", "")
        if env.get("body_encoding") == "base64":
            import base64
            data = base64.b64decode(body)
        else:
            data = body.encode()
        got = one(base, remainder, {"method": env.get("method", "POST"),
                                    "headers": env.get("headers", {}),
                                    "raw_body": data})
        got["id"] = i
        res.append(got)
    with open(a.results, "w") as f:
        json.dump(res, f)
    return a.exit_code


if __name__ == "__main__":
    sys.exit(main())

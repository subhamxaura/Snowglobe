#!/usr/bin/env python3
"""View server API: /api/manifest|events|blob|summary + bare .jsonl +
blob confinement (lexical + symlink escape) + ETag/304 + embedded page.
Stdlib only. Usage: view_api.py <snowglobe-bin> <fixture-dir>.
"""
import gzip
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "proxy"))
from sgtest_util import check_linux  # noqa: E402

SNOWGLOBE, FIXTURE = sys.argv[1], sys.argv[2]


def get(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


class Server:
    def __init__(self, target, extra=()):
        # NOTE: stderr stays a binary pipe and is drained with os.read
        # after select() — a buffered .read(n) would block for n bytes
        # and hang the test (the URL line is far shorter than 4096).
        self.proc = subprocess.Popen(
            [SNOWGLOBE, "view", target, "--port=0", *extra],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.url = self._wait_url()

    def _wait_url(self):
        import select
        t0 = time.monotonic()
        err = b""
        while time.monotonic() - t0 < 15:
            if self.proc.poll() is not None:
                rest = self.proc.stderr.read() or b""
                raise RuntimeError("view exited early: " +
                                   (err + rest).decode(errors="replace"))
            r, _, _ = select.select([self.proc.stderr], [], [], 0.2)
            if r:
                chunk = os.read(self.proc.stderr.fileno(), 4096)
                if not chunk:
                    continue
                err += chunk
                m = re.search(rb"http://127\.0\.0\.1:\d+", err)
                if m:
                    url = m.group(0).decode()
                    # Drain readiness: poll /api/manifest.
                    for _ in range(100):
                        try:
                            s, _, _ = get(url + "/api/manifest")
                            if s == 200:
                                return url
                        except OSError:
                            time.sleep(0.1)
                    raise RuntimeError("view never became ready: " +
                                       err.decode(errors="replace"))
        raise RuntimeError("view never printed a URL: " +
                           err.decode(errors="replace"))

    def stop(self):
        try:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(timeout=10)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


def check(cond, msg):
    if not cond:
        print("FAIL: " + msg)
        return False
    return True


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-view-")
    ok = True
    srv = None
    try:
        run = os.path.join(work, "run.sgr")
        shutil.copytree(FIXTURE, run, ignore=shutil.ignore_patterns("README.md"))
        with open(os.path.join(run, "events.jsonl")) as f:
            nlines = sum(1 for l in f if l.strip())
        # Symlink escape: <run>/evil -> /etc/hostname must 404.
        try:
            os.symlink("/etc/hostname", os.path.join(run, "evil"))
        except OSError:
            pass
        srv = Server(run)
        u = srv.url

        s, _, b = get(u + "/api/manifest")
        m = json.loads(b)
        ok &= check(s == 200 and m["event_count"] == nlines,
                    "manifest event_count %s want %d" % (b[:100], nlines))

        s, _, b = get(u + "/api/events?from=0&to=2")
        p = json.loads(b)
        ok &= check(s == 200 and p["total"] == nlines and len(p["events"]) == 2,
                    "page 0..2: %s" % b[:120])

        s, _, _ = get(u + "/api/events?from=0&to=6000")
        ok &= check(s == 400, "page >5000 must 400, got %d" % s)
        s, _, _ = get(u + "/api/events?from=abc&to=2")
        ok &= check(s == 400, "bad from must 400, got %d" % s)
        s, _, _ = get(u + "/api/events?from=999999999&to=1000000000")
        ok &= check(s == 416, "from past end must 416, got %d" % s)
        s, _, _ = get(u + "/api/events?from=5&to=3")
        ok &= check(s == 400, "to < from must 400, got %d" % s)

        s, h, _ = get(u + "/api/events?from=0&to=1")
        etag = h.get("ETag") or h.get("Etag")
        ok &= check(s == 200 and etag, "events ETag present")
        if etag:
            s, _, _ = get(u + "/api/events?from=0&to=1",
                           {"If-None-Match": etag})
            ok &= check(s == 304, "ETag revalidate must 304, got %d" % s)

        s, _, b = get(u + "/api/blob/llm/0000.req.json")
        ok &= check(s == 200 and b'"method"' in b, "blob req.json")
        s, _, b = get(u + "/trace/llm/0000.req.json")
        ok &= check(s == 200, "compat /trace/ alias, got %d" % s)
        s, _, _ = get(u + "/api/blob/../manifest.json")
        ok &= check(s == 404, "lexical .. escape must 404, got %d" % s)
        s, _, _ = get(u + "/api/blob/evil")
        ok &= check(s == 404, "symlink escape must 404, got %d" % s)

        s, _, b = get(u + "/api/summary")
        sm = json.loads(b)
        ok &= check(s == 200 and sm["events"] == nlines and sm["turns"] == 3
                    and sm["error_turns"] == 0,
                    "summary turns/errors: %s" % b[:200])
        ok &= check("llm.request" in sm["kinds"] and sm["files"]["written"] >= 1,
                    "summary kinds/files: %s" % b[:200])

        # /api/links: 404 without links.json (client falls back), 200
        # after `snowglobe link` — read per request, no restart needed.
        s, _, _ = get(u + "/api/links")
        ok &= check(s == 404, "links absent must 404, got %d" % s)
        r = subprocess.run([SNOWGLOBE, "link", run],
                           capture_output=True, text=True, timeout=60)
        ok &= check(r.returncode == 0, "link run: %s" % r.stderr[-200:])
        s, _, b = get(u + "/api/links")
        lm = json.loads(b) if s == 200 else {}
        ok &= check(s == 200 and lm.get("version") == 1
                    and len(lm.get("turns", [])) == 3,
                    "links 3 turns: %s" % b[:200])

        # /api/replay: 404 without replay-report.json, 200 after one is
        # planted (read per request, no restart needed).
        s, _, _ = get(u + "/api/replay")
        ok &= check(s == 404, "replay absent must 404, got %d" % s)
        with open(os.path.join(run, "replay-report.json"), "w") as f:
            json.dump({"version": 1, "original": run, "replay": run,
                       "original_exit": 0, "replay_exit": 0,
                       "turns": {"original": 3, "replay": 3, "match": True},
                       "order_matches": True, "unrecorded": 0,
                       "ignores": [],
                       "categories": {"llm": {"status": "identical",
                                              "detail": "t"}}}, f)
        s, _, b = get(u + "/api/replay")
        rm = json.loads(b) if s == 200 else {}
        ok &= check(s == 200 and rm.get("version") == 1
                    and rm["categories"]["llm"]["status"] == "identical",
                    "replay report: %s" % b[:200])
        srv.stop()
        srv = None

        # Bare .jsonl file: synthesized manifest, same events, no blobs.
        srv = Server(os.path.join(run, "events.jsonl"))
        u = srv.url
        s, _, b = get(u + "/api/manifest")
        m = json.loads(b)
        ok &= check(s == 200 and m["event_count"] == nlines
                    and m.get("synthesized") is True,
                    "bare jsonl manifest: %s" % b[:160])
        s, _, b = get(u + "/api/events?from=0&to=1")
        ok &= check(s == 200 and json.loads(b)["total"] == nlines,
                    "bare jsonl events")
        s, _, _ = get(u + "/api/blob/llm/0000.req.json")
        ok &= check(s == 404, "bare jsonl has no blobs, got %d" % s)
        srv.stop()
        srv = None

        # Embedded viewer: HTML with gzip + ETag + 304.
        srv = Server(run)
        u = srv.url
        s, h, b = get(u + "/")
        ce = h.get("Content-Encoding") or h.get("Content-encoding") or ""
        body = gzip.decompress(b) if ce == "gzip" else b
        is_html = b"<" in body[:500]
        ok &= check(s == 200 and ce == "gzip" and is_html,
                    "embedded / serves gzipped viewer (status %d, ce=%s, %d bytes)"
                    % (s, ce, len(body)))
        etag = h.get("ETag") or h.get("Etag")
        ok &= check(bool(etag), "embedded ETag present")
        if etag:
            s, _, _ = get(u + "/", {"If-None-Match": etag})
            ok &= check(s == 304, "embedded revalidate 304, got %d" % s)
        srv.stop()
        srv = None

        # Probe mirror (model.ts isProbeRequest): a bodyless POST whose
        # events omit the model key entirely is still a probe — it must
        # never count as a turn in /api/summary.
        probe_path = os.path.join(work, "probe.jsonl")
        with open(probe_path, "w") as f:
            f.write(json.dumps({"ev": "llm.request", "id": 9, "method": "POST",
                                "path": "/v1/messages", "provider": "anthropic",
                                "bytes": 0, "stream": False}) + "\n")
            f.write(json.dumps({"ev": "llm.response", "id": 9, "status": 200,
                                "bytes": 0}) + "\n")
        srv = Server(probe_path)
        u = srv.url
        s, _, b = get(u + "/api/summary")
        sm = json.loads(b)
        ok &= check(s == 200 and sm["turns"] == 0 and sm["probe_requests"] == 1
                    and sm["events"] == 2,
                    "absent-model bodyless POST is a probe: %s" % b[:200])
        srv.stop()
        srv = None

        # Bad run path exits 64.
        r = subprocess.run([SNOWGLOBE, "view", os.path.join(work, "nope")],
                           capture_output=True, text=True, timeout=30)
        ok &= check(r.returncode == 64, "bad run must exit 64, got %d" % r.returncode)

        print("PASS view_api" if ok else "FAIL view_api")
        return 0 if ok else 1
    finally:
        if srv is not None:
            srv.stop()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

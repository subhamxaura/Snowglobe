#!/usr/bin/env python3
"""Traced helper: fire N concurrent streaming POSTs through the proxy.

Runs UNDER `snowglobe run`. NOT a CTest test.
Usage: conc_client.py --results FILE --count N
Writes JSON [{status, body_sha256}] in completion order with an index:
[{i, status, body_sha256}]. All bodies are drained.
"""
import argparse
import hashlib
import http.client
import json
import os
import sys
import threading
import urllib.parse

BODY = json.dumps({"model": "mock-model-1", "stream": True}).encode()


def one(base, path, i, out):
    u = urllib.parse.urlparse(base.rstrip("/") + path)
    c = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=120)
    c.request("POST", u.path or "/", body=BODY,
              headers={"Content-Type": "application/json"})
    r = c.getresponse()
    buf = bytearray()
    while True:
        b = r.read(65536)
        if not b:
            break
        buf += b
    out[i] = {"i": i, "status": r.status,
              "body_sha256": hashlib.sha256(bytes(buf)).hexdigest(),
              "body_bytes": len(buf)}
    c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--count", type=int, default=8)
    a = ap.parse_args()
    proxy = os.environ.get("OPENAI_BASE_URL")
    if not proxy:
        print("conc_client: $OPENAI_BASE_URL is not set", file=sys.stderr)
        return 2
    out = [None] * a.count
    errs = [None] * a.count

    def run(i):
        try:
            one(proxy, "/v1/chat/completions", i, out)
        except Exception as e:  # loud, never silent
            errs[i] = repr(e)

    ts = [threading.Thread(target=run, args=(i,)) for i in range(a.count)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    bad = [e for e in errs if e]
    if bad or any(o is None for o in out):
        print("conc_client failures: %s" % bad, file=sys.stderr)
        return 1
    with open(a.results, "w") as f:
        json.dump(out, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())

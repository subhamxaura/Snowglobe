#!/usr/bin/env python3
"""Traced helper for the latency test: paired direct-vs-proxy TTFB.

Runs UNDER `snowglobe run` ($OPENAI_BASE_URL injected). NOT a CTest test.
Usage: lat_client.py --mock-url URL --results FILE --iters N
Job 0 is the delayed stream (proves first-byte-before-last); jobs 1..N are
paired quick streams. Prints nothing; writes JSON {direct, proxy, first}.
"""
import argparse
import http.client
import json
import os
import sys
import time
import urllib.parse

BODY = json.dumps({"model": "mock-model-1", "stream": True}).encode()


def ttfb(base, path):
    u = urllib.parse.urlparse(base.rstrip("/") + path)
    c = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=30)
    t0 = time.monotonic()
    c.request("POST", u.path or "/", body=BODY,
              headers={"Content-Type": "application/json"})
    r = c.getresponse()
    first = r.read(1)
    t1 = time.monotonic()
    if not first:
        raise RuntimeError("empty body from " + base + path)
    rest = r.read()  # drain so the server/proxy finish cleanly
    c.close()
    return t1 - t0, first + rest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mock-url", required=True)
    ap.add_argument("--results", required=True)
    ap.add_argument("--iters", type=int, default=20)
    a = ap.parse_args()
    proxy = os.environ.get("OPENAI_BASE_URL")
    if not proxy:
        print("lat_client: $OPENAI_BASE_URL is not set", file=sys.stderr)
        return 2
    path = "/v1/chat/completions"
    out = {"direct": [], "proxy": [], "first": None}
    tp, _ = ttfb(proxy, path)  # delayed stream: first byte must beat 2 s
    out["first"] = tp
    for _ in range(a.iters):
        td, _ = ttfb(a.mock_url, path)
        tp, _ = ttfb(proxy, path)
        out["direct"].append(td)
        out["proxy"].append(tp)
    with open(a.results, "w") as f:
        json.dump(out, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Traced helper: one normal error, one mid-stream disconnect, one
unreachable-upstream request, then exit. Runs UNDER `snowglobe run`.
NOT a CTest test. Usage: notmp_client.py --mock-url URL --closed-b64 B64
--results FILE. Writes [{status}] in order.
"""
import argparse
import hashlib
import http.client
import json
import os
import sys
import time
import urllib.parse

BODY = json.dumps({"model": "mock-model-1", "stream": True}).encode()


def post(base, path, body=None, headers=None):
    u = urllib.parse.urlparse(base.rstrip("/") + path)
    c = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=60)
    data = body if body is not None else BODY
    c.request("POST", u.path or "/", body=data,
              headers={"Content-Type": "application/json", **(headers or {})})
    r = c.getresponse()
    buf = r.read()
    out = (r.status, hashlib.sha256(buf).hexdigest(), len(buf))
    c.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--closed-b64", required=True)
    ap.add_argument("--results", required=True)
    a = ap.parse_args()
    proxy = os.environ.get("OPENAI_BASE_URL")
    if not proxy:
        print("notmp_client: $OPENAI_BASE_URL is not set", file=sys.stderr)
        return 2
    res = []
    # 1. Normal-path non-2xx (429 scripted first on the mock).
    st, _, _ = post(proxy, "/chat/completions",
                    json.dumps({"model": "mock-model-1", "stream": False}).encode())
    res.append({"status": st})
    # 2. Mid-stream disconnect: read a sip of the long stream, then close.
    #    The mock's stream is finite (20 x 25 ms) so the proxy's join ends.
    u = urllib.parse.urlparse(proxy.rstrip("/") + "/chat/completions")
    c = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=60)
    c.request("POST", u.path or "/", body=BODY,
              headers={"Content-Type": "application/json"})
    r = c.getresponse()
    if not r.read(32):
        print("notmp_client: no bytes before disconnect", file=sys.stderr)
        return 1
    c.close()
    time.sleep(3)
    # 3. Unreachable upstream via /u/ (closed port): synthesized 502.
    #    /u/ hangs off the proxy ORIGIN, not the /openai/v1 base.
    origin = proxy.rsplit("/", 2)[0]
    pu = urllib.parse.urlparse(origin + "/u/" + a.closed_b64 + "/x")
    c3 = http.client.HTTPConnection(pu.hostname, pu.port or 80, timeout=60)
    c3.request("POST", pu.path or "/", body=BODY,
               headers={"Content-Type": "application/json"})
    r3 = c3.getresponse()
    b3 = r3.read()
    res.append({"disconnect": True})
    res.append({"status": r3.status, "bytes": len(b3)})
    c3.close()
    with open(a.results, "w") as f:
        json.dump(res, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())

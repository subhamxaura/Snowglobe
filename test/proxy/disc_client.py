#!/usr/bin/env python3
"""Traced helper: disconnect mid-stream, then prove the proxy is healthy.

Runs UNDER `snowglobe run`. NOT a CTest test.
Usage: disc_client.py --results FILE
Opens a 200-chunk streamed response, reads head + a sip, then closes the
socket abruptly. Waits 7 s (the mock's remaining ~5 s drain past the proxy's
join), then issues one normal request. Writes {second: {status, bytes}}.
"""
import argparse
import http.client
import json
import os
import sys
import time
import urllib.parse

BODY = json.dumps({"model": "mock-model-1", "stream": True}).encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    a = ap.parse_args()
    proxy = os.environ.get("OPENAI_BASE_URL")
    if not proxy:
        print("disc_client: $OPENAI_BASE_URL is not set", file=sys.stderr)
        return 2
    u = urllib.parse.urlparse(proxy.rstrip("/") + "/v1/chat/completions")
    c = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=30)
    c.request("POST", u.path or "/", body=BODY,
              headers={"Content-Type": "application/json"})
    r = c.getresponse()
    sip = r.read(32)
    if not sip:
        print("disc_client: no bytes before disconnect", file=sys.stderr)
        return 1
    c.close()  # abrupt mid-stream close; proxy must record truncated:true
    time.sleep(7)
    c2 = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=30)
    c2.request("POST", u.path or "/", body=BODY,
               headers={"Content-Type": "application/json"})
    r2 = c2.getresponse()
    body = r2.read()
    out = {"second": {"status": r2.status, "bytes": len(body)}}
    c2.close()
    with open(a.results, "w") as f:
        json.dump(out, f)
    return 0 if r2.status == 200 else 1


if __name__ == "__main__":
    sys.exit(main())

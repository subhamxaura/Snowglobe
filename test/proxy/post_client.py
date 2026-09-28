#!/usr/bin/env python3
"""Traced helper: POST a list of jobs through the proxy, print results.

Runs UNDER `snowglobe run` (needs injected *BASE_URL env). NOT a CTest
test itself.
Usage: post_client.py --jobs FILE --results FILE [--base-env NAME]
jobs: [{path, body (obj) or body_file, headers}] — path appended to the
  base URL from $BASE_ENV (default OPENAI_BASE_URL).
results: [{status, body_sha256, body_bytes}] as JSON. Non-2xx are recorded,
not raised. Reads fully (streams drained) so the proxy always finishes.
"""
import argparse
import hashlib
import http.client
import json
import os
import sys
import urllib.parse


def one(base, job):
    u = urllib.parse.urlparse(base.rstrip("/") + job["path"])
    host, port = u.hostname, u.port or 80
    if isinstance(job.get("body"), dict) and "body_file" in job:
        raise ValueError("body and body_file are exclusive")
    if "body_file" in job:
        with open(job["body_file"], "rb") as f:
            data = f.read()
    else:
        data = json.dumps(job.get("body", {})).encode()
    conn = http.client.HTTPConnection(host, port, timeout=120)
    conn.request("POST", u.path or "/", body=data,
                 headers={"Content-Type": "application/json",
                          **job.get("headers", {})})
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
    ap.add_argument("--jobs", required=True)
    ap.add_argument("--results", required=True)
    ap.add_argument("--base-env", default="OPENAI_BASE_URL")
    a = ap.parse_args()
    base = os.environ.get(a.base_env)
    if not base:
        print("post_client: $%s is not set" % a.base_env, file=sys.stderr)
        return 2
    with open(a.jobs) as f:
        jobs = json.load(f)
    res = [one(base, j) for j in jobs]
    with open(a.results, "w") as f:
        json.dump(res, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())

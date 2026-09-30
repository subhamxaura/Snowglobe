#!/usr/bin/env python3
"""9(c): 8 concurrent streams with pairwise-distinct bodies — every
stored response body must equal, byte for byte, the body the mock sent AND
the body its own client thread received (multiset bijections over 8
distinct sha256: cross-stream contamination cannot hide, unlike with
identical bodies). All status 200.
"""
import collections
import glob
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import (PROXY_DIR, check_linux, free_port, Mock, load_events,
                         llm_pairs, run_sg, sha_file)

SNOWGLOBE, MOCKLLM = sys.argv[1], sys.argv[2]
N = 8


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-conc-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        # Pairwise-distinct bodies (stream index embedded): with identical
        # bodies a cross-stream mix-up is invisible; distinct shas force a
        # 1:1 correspondence stored<->sent and stored<->received.
        scenario = {"responses": [
            {"status": 200, "stream": True,
             "chunks": ["data: {\"s\":%d,\"k\":%d}\n\n" % (j, i)
                        for i in range(10)]}
            for j in range(N)]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        res = os.path.join(work, "conc.json")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "conc_client.py"),
                    "--results", res, "--count", str(N)],
                   out, timeout=180)
        if r.returncode != 0:
            print("FAIL: snowglobe run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res) as f:
            got = json.load(f)
        if len(got) != N or any(g["status"] != 200 for g in got):
            print("FAIL: client results: %s" % got)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if len(reqs) != N or len(resps) != N:
            print("FAIL: want %d pairs, got %d req %d res"
                  % (N, len(reqs), len(resps)))
            return 1
        stored = collections.Counter()
        for i, s in resps.items():
            if s["status"] != 200 or s["truncated"]:
                print("FAIL: bad response %s" % s)
                return 1
            stored[sha_file(os.path.join(out, s["res"]))] += 1
        sent_hashes = collections.Counter()
        for p in glob.glob(os.path.join(mock.sent, "res-*.bin")):
            sent_hashes[sha_file(p)] += 1
        if stored != sent_hashes or any(v != 1 for v in stored.values()):
            print("FAIL: stored %s != sent %s (want 8 distinct 1:1)"
                  % (stored, sent_hashes))
            return 1
        received = collections.Counter(g["body_sha256"] for g in got)
        if received != stored:
            print("FAIL: per-thread received %s != stored %s" % (received, stored))
            return 1
        print("PASS concurrent: %d streams pairwise byte-exact" % N)
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

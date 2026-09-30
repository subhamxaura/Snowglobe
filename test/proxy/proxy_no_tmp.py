#!/usr/bin/env python3
"""No-*.tmp invariant: after a run covering the normal error path (429),
a mid-stream client disconnect, and an unreachable-upstream 502, llm/
holds zero *.tmp files and every id has its final blobs. (A real Claude
Code run once left 0000.res.tmp + 0000.res.idx.tmp behind on the 502
path, which opened .tmps and then wrote finals directly.)
"""
import base64
import glob
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import (PROXY_DIR, check_linux, free_port, Mock, load_events,
                         llm_pairs, run_sg)

SNOWGLOBE, MOCKLLM = sys.argv[1], sys.argv[2]


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-notmp-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        closed = free_port()
        long_stream = {"status": 200, "stream": True, "chunk_delay_ms": 25,
                       "chunks": ["data: {\"k\":%d}\n\n" % i for i in range(20)]}
        scenario = {"responses": [
            {"status": 429, "stream": False, "body": {"error": "slow"}},
            long_stream,
        ]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        b64 = base64.urlsafe_b64encode(
            ("http://127.0.0.1:%d" % closed).encode()).decode().rstrip("=")
        res = os.path.join(work, "notmp.json")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "notmp_client.py"),
                    "--closed-b64", b64, "--results", res],
                   out, timeout=180)
        if r.returncode != 0:
            print("FAIL: snowglobe run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res) as f:
            m = json.load(f)
        if m[0]["status"] != 429 or m[2]["status"] != 502:
            print("FAIL: client results: %s" % m)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if sorted(resps) != [0, 1, 2]:
            print("FAIL: want res ids [0,1,2], got %s" % sorted(resps))
            return 1
        if resps[0]["status"] != 429 or resps[2]["status"] != 502:
            print("FAIL: recorded statuses wrong")
            return 1
        if resps[1]["truncated"] is not True:
            print("FAIL: disconnect response not truncated")
            return 1
        tmps = glob.glob(os.path.join(out, "llm", "*.tmp"))
        if tmps:
            print("FAIL: leftover tmp files: %s" % tmps)
            return 1
        for i, s in resps.items():
            for k in ("req", "res", "idx"):
                if not os.path.exists(os.path.join(out, s[k])):
                    print("FAIL: missing blob %s for id %d" % (k, i))
                    return 1
        print("PASS no_tmp: 429 + disconnect + 502 leave zero *.tmp")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

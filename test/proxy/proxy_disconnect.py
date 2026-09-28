#!/usr/bin/env python3
"""9(d): client disconnect mid-stream records truncated:true and the next
request on the same proxy succeeds with 200.
"""
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
    work = tempfile.mkdtemp(prefix="sg-disc-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        long_stream = {"status": 200, "stream": True, "chunk_delay_ms": 25,
                       "chunks": ["data: {\"k\":%d}\n\n" % i
                                  for i in range(200)]}
        quick = {"status": 200, "stream": False,
                 "body": {"id": "ok", "choices": []}}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port,
                    {"responses": [long_stream, quick]})
        res = os.path.join(work, "disc.json")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "disc_client.py"),
                    "--results", res],
                   out, timeout=180)
        if r.returncode != 0:
            print("FAIL: snowglobe run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res) as f:
            m = json.load(f)
        if m["second"]["status"] != 200:
            print("FAIL: post-disconnect request: %s" % m)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if sorted(reqs) != [0, 1] or sorted(resps) != [0, 1]:
            print("FAIL: want ids [0,1], got req=%s res=%s"
                  % (sorted(reqs), sorted(resps)))
            return 1
        if resps[0]["truncated"] is not True:
            print("FAIL: disconnect response not truncated:true: %s"
                  % resps[0])
            return 1
        if resps[1]["status"] != 200 or resps[1]["truncated"]:
            print("FAIL: second response bad: %s" % resps[1])
            return 1
        print("PASS disconnect: truncated:true then 200")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

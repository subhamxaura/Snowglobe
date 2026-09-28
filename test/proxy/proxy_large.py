#!/usr/bin/env python3
"""9(f): 50 MB request (buffered upload, forwarded intact — the mock's
received sha must equal what the client sent) and 50 MB streamed response
(chunked download, zero application buffering — stored blob sha must equal
what the mock sent).
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import (PROXY_DIR, check_linux, free_port, Mock, load_events,
                         llm_pairs, run_sg, sha_file)

SNOWGLOBE, MOCKLLM = sys.argv[1], sys.argv[2]
N = 50 * 1024 * 1024


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-large-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        scenario = {"responses": [
            {"status": 200, "stream": False,
             "body": {"id": "big-ack", "ok": True}},
            {"status": 200, "stream": True,
             "chunks_gen": {"count": 200, "bytes_each": 262144}},
        ]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        big = os.path.join(work, "big.json")
        pre = '{"model":"mock-model-1","stream":false,"data":"'
        suf = '"}'
        with open(big, "w") as f:
            f.write(pre + "Q" * (N - len(pre) - len(suf)) + suf)
        with open(big, "rb") as f:
            sent_sha = hashlib.sha256(f.read()).hexdigest()
        jobs = [{"path": "/chat/completions", "body_file": big},
                {"path": "/chat/completions",
                 "body": {"model": "mock-model-1", "stream": True}}]
        jobs_f = os.path.join(work, "jobs.json")
        res_f = os.path.join(work, "results.json")
        with open(jobs_f, "w") as f:
            json.dump(jobs, f)
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "post_client.py"),
                    "--jobs", jobs_f, "--results", res_f],
                   out, timeout=300)
        if r.returncode != 0:
            print("FAIL: snowglobe run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res_f) as f:
            got = json.load(f)
        if got[0]["status"] != 200 or got[1]["status"] != 200:
            print("FAIL: client results: %s" % got)
            return 1
        recs = mock.header_records()
        if len(recs) != 2 or recs[0]["body_sha256"] != sent_sha:
            print("FAIL: mock received sha != sent sha: %s vs %s"
                  % ([x.get("body_sha256") for x in recs], sent_sha))
            return 1
        if recs[0]["body_bytes"] != N:
            print("FAIL: mock received %d bytes, want %d"
                  % (recs[0]["body_bytes"], N))
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if sorted(resps) != [0, 1]:
            print("FAIL: want res ids [0,1], got %s" % sorted(resps))
            return 1
        if reqs[0]["bytes"] != N:
            print("FAIL: recorded request bytes %d != %d"
                  % (reqs[0]["bytes"], N))
            return 1
        res1 = os.path.join(out, resps[1]["res"])
        if sha_file(res1) != got[1]["body_sha256"]:
            print("FAIL: stored 50MB response != client bytes")
            return 1
        import glob as g
        sent = sorted(g.glob(os.path.join(mock.sent, "res-*.bin")))
        if len(sent) != 2 or sha_file(sent[1]) != got[1]["body_sha256"]:
            print("FAIL: mock sent files inconsistent")
            return 1
        if sha_file(res1) != sha_file(sent[1]):
            print("FAIL: stored response != mock sent bytes")
            return 1
        print("PASS large: 50MB up intact, 50MB down byte-exact "
              "(%d response bytes)" % got[1]["body_bytes"])
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

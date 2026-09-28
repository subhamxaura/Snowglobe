#!/usr/bin/env python3
"""9(e): 429 then 500 pass through with status + body byte-exact, and both
are recorded as llm.response with their real statuses (turns counts only
2xx, so the epilogue reports 0 LLM turns).
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


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-err-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        b429 = {"error": {"message": "slow down", "code": "rate_limit"}}
        b500 = {"error": {"message": "boom"}}
        scenario = {"responses": [
            {"status": 429, "stream": False, "body": b429},
            {"status": 500, "stream": False, "body": b500},
        ]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        jobs = [{"path": "/v1/chat/completions",
                 "body": {"model": "mock-model-1", "stream": False}},
                {"path": "/v1/chat/completions",
                 "body": {"model": "mock-model-1", "stream": False}}]
        jobs_f = os.path.join(work, "jobs.json")
        res_f = os.path.join(work, "results.json")
        with open(jobs_f, "w") as f:
            json.dump(jobs, f)
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "post_client.py"),
                    "--jobs", jobs_f, "--results", res_f],
                   out, timeout=120)
        if r.returncode != 0:
            print("FAIL: post_client exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res_f) as f:
            got = json.load(f)
        want = [(429, b429), (500, b500)]
        for g, (st, body) in zip(got, want):
            h = hashlib.sha256(json.dumps(body).encode()).hexdigest()
            if g["status"] != st or g["body_sha256"] != h:
                print("FAIL: client got %s, want status %d sha %s"
                      % (g, st, h))
                return 1
        if "0 LLM turns" not in r.stderr:
            print("FAIL: epilogue should report 0 LLM turns:\n" + r.stderr)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if sorted(resps) != [0, 1]:
            print("FAIL: want res ids [0,1], got %s" % sorted(resps))
            return 1
        if resps[0]["status"] != 429 or resps[1]["status"] != 500:
            print("FAIL: recorded statuses wrong: %s %s"
                  % (resps[0], resps[1]))
            return 1
        for i, (_, body) in enumerate(want):
            p = os.path.join(out, resps[i]["res"])
            if sha_file(p) != hashlib.sha256(json.dumps(body).encode()
                                             ).hexdigest():
                print("FAIL: stored error body %d not byte-exact" % i)
                return 1
        print("PASS errors: 429 + 500 passthrough byte-exact, 0 turns")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

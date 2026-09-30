#!/usr/bin/env python3
"""Single error turn: mock returns 402 → the epilogue counts it
("1 LLM turn (1 error)"), and llm.response carries status 402 with the
error body blob. Non-2xx responses ARE turns (a real Claude Code run with
two error responses once reported "0 LLM turns").
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
    work = tempfile.mkdtemp(prefix="sg-402-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        body = {"error": {"message": "credit balance too low", "type": "auth_error"}}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port,
                    {"responses": [{"status": 402, "stream": False, "body": body}]})
        jobs = [{"path": "/chat/completions",
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
            print("FAIL: snowglobe run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res_f) as f:
            got = json.load(f)
        want_sha = hashlib.sha256(json.dumps(body).encode()).hexdigest()
        if len(got) != 1 or got[0]["status"] != 402 or got[0]["body_sha256"] != want_sha:
            print("FAIL: client results: %s" % got)
            return 1
        if "1 LLM turn (1 error)" not in r.stderr:
            print("FAIL: epilogue should report '1 LLM turn (1 error)':\n" + r.stderr)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if sorted(resps) != [0]:
            print("FAIL: want res id [0], got %s" % sorted(resps))
            return 1
        if resps[0]["status"] != 402 or resps[0]["truncated"]:
            print("FAIL: bad response event: %s" % resps[0])
            return 1
        if sha_file(os.path.join(out, resps[0]["res"])) != want_sha:
            print("FAIL: stored 402 body not byte-exact")
            return 1
        print("PASS error_turn: 402 counts as 1 turn with 1 error")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

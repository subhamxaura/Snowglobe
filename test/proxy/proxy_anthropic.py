#!/usr/bin/env python3
"""Anthropic Messages path: non-stream + streamed responses through
/proxy/anthropic → mock, x-api-key forwarded intact and stored REDACTED.
(Covers the mock's second provider and the proxy's header set beyond
Authorization.)
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
KEY = "sk-ant-test-anthropic-002"


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-anth-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        scenario = {"responses": [
            {"status": 200, "stream": False, "body": {
                "id": "msg_1", "model": "mock-claude-1",
                "content": [{"type": "text", "text": "hi"}]}},
            {"status": 200, "stream": True,
             "chunks": ["data: {\"t\":\"a\"}\n\n", "data: {\"t\":\"b\"}\n\n"]},
        ]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        jobs = [{"path": "/v1/messages",
                 "body": {"model": "mock-claude-1", "messages": [],
                          "max_tokens": 8},
                 "headers": {"x-api-key": KEY, "anthropic-version": "2023-06-01"}},
                {"path": "/v1/messages",
                 "body": {"model": "mock-claude-1", "stream": True,
                          "messages": [], "max_tokens": 8},
                 "headers": {"x-api-key": KEY, "anthropic-version": "2023-06-01"}}]
        jobs_f = os.path.join(work, "jobs.json")
        res_f = os.path.join(work, "results.json")
        with open(jobs_f, "w") as f:
            json.dump(jobs, f)
        r = run_sg(SNOWGLOBE,
                   ["--upstream=anthropic=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "post_client.py"),
                    "--jobs", jobs_f, "--results", res_f,
                    "--base-env", "ANTHROPIC_BASE_URL"],
                   out, timeout=120)
        if r.returncode != 0:
            print("FAIL: post_client exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res_f) as f:
            got = json.load(f)
        if any(g["status"] != 200 for g in got):
            print("FAIL: client results: %s" % got)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if sorted(reqs) != [0, 1]:
            print("FAIL: want req ids [0,1], got %s" % sorted(reqs))
            return 1
        if any(q["provider"] != "anthropic" for q in reqs.values()):
            print("FAIL: provider not anthropic: %s" % reqs)
            return 1
        with open(os.path.join(out, "llm", "0000.req.json")) as f:
            stored = json.load(f)
        if stored["headers"].get("x-api-key") != "REDACTED":
            print("FAIL: stored x-api-key not REDACTED: %s" % stored["headers"])
            return 1
        seen = [h["headers"].get("x-api-key") for h in mock.header_records()]
        if seen != [KEY, KEY]:
            print("FAIL: mock did not see the real x-api-key x2: %s" % seen)
            return 1
        print("PASS anthropic: 2 pairs, x-api-key REDACTED/stored intact fwd")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

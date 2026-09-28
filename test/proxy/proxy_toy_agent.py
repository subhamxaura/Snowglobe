#!/usr/bin/env python3
"""9(a): toy agent through the proxy — 3 llm.request/response pairs
interleaved with proc.exec, fs.open(write) and net.connect to the mock;
stored request headers read REDACTED while the mock saw the real
Authorization; the run epilogue reports 3 LLM turns.
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import (PROXY_DIR, check_linux, free_port, Mock, grep_bytes,
                         load_events, llm_pairs, run_sg, skip)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
KEY = "sk-test-toyagent-001"


def tc(i, name, args):
    return {"id": "call_%d" % i, "type": "function",
            "function": {"name": name, "arguments": json.dumps(args)}}


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-toy-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        wd = os.path.join(work, "w")
        os.makedirs(wd)
        durl = "http://127.0.0.1:%d/test-data" % port
        scenario = {"responses": [
            {"status": 200, "stream": False, "body": {
                "id": "chatcmpl-1", "model": "mock-model-1",
                "choices": [{"message": {"role": "assistant", "content": None,
                                         "tool_calls": [tc(
                                             1, "write_file",
                                             {"path": wd + "/note.txt",
                                              "content": "hello snowglobe"})]}}]}},
            {"status": 200, "stream": False, "body": {
                "id": "chatcmpl-2", "model": "mock-model-1",
                "choices": [{"message": {"role": "assistant", "content": None,
                                         "tool_calls": [
                                             tc(2, "run_command", {"command":
                                                                   "echo tool-output-123"}),
                                             tc(3, "http_get", {"url": durl})]}}]}},
            {"status": 200, "stream": False, "body": {
                "id": "chatcmpl-3", "model": "mock-model-1", "choices": [
                    {"message": {"role": "assistant",
                                 "content": "done"}}]}},
        ]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, AGENT, "--workdir", wd,
                    "--data-url", durl],
                   out, env_extra={"OPENAI_API_KEY": KEY}, timeout=180)
        if r.returncode != 0:
            print("FAIL: snowglobe run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        if "3 LLM turns" not in r.stderr:
            print("FAIL: epilogue has no '3 LLM turns':\n" + r.stderr)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if sorted(reqs) != [0, 1, 2] or sorted(resps) != [0, 1, 2]:
            print("FAIL: want ids [0,1,2], got req=%s res=%s"
                  % (sorted(reqs), sorted(resps)))
            return 1
        for i in range(3):
            q, s = reqs[i], resps[i]
            if q["provider"] != "openai" or q["model"] != "mock-model-1":
                print("FAIL: bad request event %d: %s" % (i, q))
                return 1
            if s["status"] != 200 or s["truncated"]:
                print("FAIL: bad response event %d: %s" % (i, s))
                return 1
        if not any(e.get("ev") == "proc.exec" and
                   any("tool-output-123" in a for a in e.get("argv", []))
                   for e in evs):
            print("FAIL: no proc.exec carrying the run_command payload")
            return 1
        if not any(e.get("ev") == "fs.open" and e.get("path") == wd + "/note.txt"
                   and e.get("write") is True for e in evs):
            print("FAIL: no fs.open(write) for the note file")
            return 1
        if not any(e.get("ev") == "net.connect" and
                   str(e.get("addr", "")).endswith(":%d" % port) for e in evs):
            print("FAIL: no net.connect to the mock port %d" % port)
            return 1
        with open(os.path.join(out, "llm", "0000.req.json")) as f:
            stored = json.load(f)
        if stored["headers"].get("Authorization") != "REDACTED":
            print("FAIL: stored Authorization is not REDACTED: %s"
                  % stored["headers"])
            return 1
        seen = [h["headers"].get("authorization")
                for h in mock.header_records()]
        if seen != ["Bearer " + KEY] * 3:
            print("FAIL: mock did not see the real header x3: %s" % seen)
            return 1
        with open(os.path.join(wd, "note.txt")) as f:
            if f.read() != "hello snowglobe":
                print("FAIL: note.txt content wrong")
                return 1
        print("PASS toy_agent: 3 pairs + exec/open/connect, "
              "REDACTED stored / real forwarded, epilogue ok")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

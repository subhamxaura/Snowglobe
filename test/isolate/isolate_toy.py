#!/usr/bin/env python3
"""Phase 2 Block 1 (a): toy agent under --isolate — same 3-turn golden
shape as proxy_toy_agent, plus the isolate record; the note.txt write
lands in overlay/upper while the host workdir stays clean (the
non-isolate twin asserts the file IS there).
Usage: isolate_toy.py <snowglobe-bin> <mockllm-dir> <toyagent-dir>.
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "proxy"))
from sgtest_util import (check_linux, free_port, Mock, load_events,  # noqa: E402
                         llm_pairs, run_sg, skip)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
KEY = "sk-test-isotoy-001"


def tc(i, name, args):
    return {"id": "call_%d" % i, "type": "function",
            "function": {"name": name, "arguments": json.dumps(args)}}


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-isotoy-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        # Capability gate first: 69 here skips the whole file loudly.
        probe = os.path.join(work, "probe.sgr")
        r = run_sg(SNOWGLOBE, ["--isolate", "--", "/bin/true"], probe)
        if r.returncode == 69:
            skip("no --isolate capability here: " + r.stderr.strip()[-200:])
        if r.returncode != 0:
            print("FAIL: isolate smoke exited %d:\n%s" % (r.returncode, r.stderr))
            return 1
        port = free_port()
        wd = os.path.join(work, "w")
        os.makedirs(wd)
        durl = "http://127.0.0.1:%d/test-data" % port
        scenario = {"responses": [
            {"status": 200, "stream": False, "body": {
                "id": "chatcmpl-1", "model": "mock-model-1",
                "choices": [{"message": {"role": "assistant", "content": None,
                                         "tool_calls": [tc(
                                             1, "run_command",
                                             {"command": "echo tool-output-123"})]}}]}},
            {"status": 200, "stream": False, "body": {
                "id": "chatcmpl-2", "model": "mock-model-1",
                "choices": [{"message": {"role": "assistant", "content": None,
                                         "tool_calls": [
                                             tc(2, "write_file",
                                                {"path": wd + "/note.txt",
                                                 "content": "hello snowglobe"}),
                                             tc(3, "http_get", {"url": durl})]}}]}},
            {"status": 200, "stream": False, "body": {
                "id": "chatcmpl-3", "model": "mock-model-1", "choices": [
                    {"message": {"role": "assistant",
                                 "content": "done"}}]}},
        ]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        # The workdir doubles as --project: note.txt must copy up.
        # Bytecode caches off: the agent source lives on a read-only bind.
        r = run_sg(SNOWGLOBE,
                   ["--isolate", "--project=" + wd,
                    "--upstream=openai=" + mock.base, "--",
                    sys.executable, AGENT, "--workdir", wd,
                    "--data-url", durl],
                   out, env_extra={"OPENAI_API_KEY": KEY,
                                   "PYTHONDONTWRITEBYTECODE": "1"},
                   timeout=180)
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
        if not any(e.get("ev") == "proc.exec" and
                   any("tool-output-123" in a for a in e.get("argv", []))
                   for e in evs):
            print("FAIL: no proc.exec carrying the run_command payload")
            return 1
        if not any(e.get("ev") == "fs.open" and e.get("path") == wd + "/note.txt"
                   and e.get("write") is True for e in evs):
            print("FAIL: no fs.open(write) for the note file")
            return 1
        metas = [e for e in evs if e.get("ev") == "run.meta"]
        if len(metas) != 1 or metas[0].get("isolate") is not True:
            print("FAIL: run.meta isolate record missing")
            return 1
        with open(os.path.join(out, "manifest.json")) as f:
            iso = json.load(f).get("isolate", {})
        if iso.get("on") is not True or iso.get("upper") != "overlay/upper":
            print("FAIL: manifest isolate record wrong: %s" % iso)
            return 1
        # Overlay proof: host workdir clean, upper copy exact.
        if os.path.exists(os.path.join(wd, "note.txt")):
            print("FAIL: note.txt leaked to the host workdir")
            return 1
        with open(os.path.join(out, "overlay", "upper", "note.txt")) as f:
            if f.read() != "hello snowglobe":
                print("FAIL: upper note.txt content wrong")
                return 1
        print("PASS isolate_toy: 3-turn shape + isolate record, "
              "upper copy-up, host clean")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

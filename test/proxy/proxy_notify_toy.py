#!/usr/bin/env python3
"""Phase 4 Block 2: toy agent through the proxy under --backend=notify.

Same 3-turn flow as proxy_toy_agent (pairs, exec/open/connect,
REDACTED stored vs real forwarded, note.txt, epilogue), plus the
notify shape contract: run.meta backend:notify, every outcome-class
event carries result_known:false, no ok/errno/fd keys outside
trace.decode_error (which must be absent), proc.exec_failed never
appears. Backend preflight skips (69) with a reason.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import (PROXY_DIR, check_linux, free_port, Mock, load_events,
                         llm_pairs, run_sg, skip)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
KEY = "sk-test-notifytoy-001"
OUTCOME = {"fs.open", "fs.unlink", "fs.rmdir", "fs.rename", "fs.symlink",
           "fs.chmod", "net.connect", "net.sendto", "net.bind",
           "net.disconnect", "proc.exec"}


def tc(i, name, args):
    return {"id": "call_%d" % i, "type": "function",
            "function": {"name": name, "arguments": json.dumps(args)}}


def preflight():
    work = tempfile.mkdtemp(prefix="sg-ntoy-pre-")
    try:
        r = subprocess.run(
            [SNOWGLOBE, "run",
             "--out=" + os.path.join(work, "p.sgr"),
             "--backend=notify", "--", "/bin/true"],
            capture_output=True, text=True, timeout=60)
        if r.returncode == 69:
            skip("kernel lacks seccomp user-notify")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    check_linux(SNOWGLOBE)
    preflight()
    work = tempfile.mkdtemp(prefix="sg-ntoy-")
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
        r = run_sg(SNOWGLOBE,
                   ["--backend=notify", "--upstream=openai=" + mock.base, "--",
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
        # Notify shape contract (raw stream).
        metas = [e for e in evs if e.get("ev") == "run.meta"]
        if len(metas) != 1 or metas[0].get("backend") != "notify":
            print("FAIL: run.meta backend != notify")
            return 1
        if any(e.get("ev") == "trace.decode_error" for e in evs):
            print("FAIL: trace.decode_error present")
            return 1
        if any(e.get("ev") == "proc.exec_failed" for e in evs):
            print("FAIL: proc.exec_failed under notify (no exit values exist)")
            return 1
        for e in evs:
            ev = e.get("ev")
            if ev in OUTCOME:
                if e.get("result_known") is not False:
                    print("FAIL: %s lacks result_known:false: %s" % (ev, e))
                    return 1
            elif "result_known" in e:
                print("FAIL: unexpected result_known on %s: %s" % (ev, e))
                return 1
            for k in ("ok", "errno", "fd"):
                if k in e:
                    print("FAIL: notify %s carries %r: %s" % (ev, k, e))
                    return 1
        with open(os.path.join(out, "llm", "0000.req.json")) as f:
            stored = json.load(f)
        if stored["headers"].get("Authorization") != "REDACTED":
            print("FAIL: stored Authorization is not REDACTED")
            return 1
        with open(os.path.join(wd, "note.txt")) as f:
            if f.read() != "hello snowglobe":
                print("FAIL: note.txt content wrong")
                return 1
        print("PASS notify_toy: 3 pairs + exec/open/connect under notify, "
              "shape contract holds, secrets redacted")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

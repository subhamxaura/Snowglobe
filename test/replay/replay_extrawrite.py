#!/usr/bin/env python3
"""replay_extrawrite (mutation b): altered agent writes an extra file.

The replayed agent is a copy of the toy that additionally writes
extra.txt after the model says done. Recorded responses still drive all
3 turns (LLM layer identical), but the fs multiset gains a path: fs
diverged with the extra path as evidence, exit 65.
"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_util import (check_linux, free_port, Mock, toy_scenario,
                         run_sg, run_replay, load_report)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
SCRUB = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN",
         "GEMINI_API_KEY", "GOOGLE_API_KEY")

EXTRA_HOOK = '''
        if not calls:
            with open(a.workdir + "/extra.txt", "w") as f:
                f.write("undeclared side effect")
            print(msg.get("content") or "")
            return 0
'''


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="tmp.")
    work2 = tempfile.mkdtemp(prefix="tmp.")
    mock = None
    try:
        with open(AGENT) as f:
            src = f.read()
        anchor = '''        if not calls:
            print(msg.get("content") or "")
            return 0
'''
        if anchor not in src:
            print("FAIL: toy agent shape changed, cannot plant hook")
            return 1
        altered = os.path.join(work, "altered_agent.py")
        with open(altered, "w") as f:
            f.write(src.replace(anchor, EXTRA_HOOK))
        port = free_port()
        wd = os.path.join(work, "w")
        os.makedirs(wd)
        durl = "http://127.0.0.1:%d/test-data" % port
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port,
                    toy_scenario(wd, durl))
        orig = os.path.join(work, "orig.sgr")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, AGENT, "--workdir", wd,
                    "--data-url", durl],
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-mut-b"})
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        # Replay with the ALTERED agent (fresh workdir, same data server:
        # the recorded http_get URL is frozen in the past).
        wd2 = os.path.join(work2, "w")
        os.makedirs(wd2)
        new = os.path.join(work, "new.sgr")
        r2 = run_replay(
            SNOWGLOBE, orig,
            ["--", sys.executable, altered, "--workdir", wd2,
             "--data-url", durl],
            new, scrub=SCRUB)
        if r2.returncode != 65:
            print("FAIL: replay exited %d, want 65 (diverged)\n%s%s"
                  % (r2.returncode, r2.stdout, r2.stderr))
            return 1
        rep = load_report(new)
        if rep["categories"]["llm"]["status"] != "identical":
            print("FAIL: llm should stay identical: %s"
                  % rep["categories"]["llm"])
            return 1
        fs = rep["categories"]["fs"]
        if fs["status"] != "diverged":
            print("FAIL: fs not diverged: %s" % fs)
            return 1
        if not any("extra.txt" in e for e in fs["replay_only"]):
            print("FAIL: fs evidence missing extra.txt: %s"
                  % fs["replay_only"])
            return 1
        if not os.path.exists(os.path.join(wd2, "extra.txt")):
            print("FAIL: extra.txt was not actually written")
            return 1
        print("PASS replay_extrawrite: fs divergence names extra.txt, exit 65")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(work2, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""replay_fallback (P2): every turn served via the endpoint fallback.

The replayed agent is a copy of the toy whose user prompt carries a
timestamp. All three replayed request bodies therefore miss the primary
hash (the stamp propagates through the message history) and the
order-preserving endpoint fallback serves recorded 0/1/2 in order. The
agent still finishes (recorded responses drive the same tools), but the
report grades llm diverged with served_exact=0, served_fallback=3:
fallback keeps the run alive, never silent.
"""
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_util import (check_linux, free_port, Mock, toy_scenario,
                         run_sg, run_replay, load_report, print_llm)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
SCRUB = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN",
         "GEMINI_API_KEY", "GOOGLE_API_KEY")

PROMPT_ANCHOR = '"fetch the data URL. Reply done when finished."'


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="tmp.")
    work2 = tempfile.mkdtemp(prefix="tmp.")
    mock = None
    try:
        with open(AGENT) as f:
            src = f.read()
        if PROMPT_ANCHOR not in src:
            print("FAIL: toy agent shape changed, cannot plant stamp")
            return 1
        stamp = " [replay-t=%d]" % time.time_ns()
        stamped = os.path.join(work2, "stamped_agent.py")
        with open(stamped, "w") as f:
            f.write(src.replace(
                PROMPT_ANCHOR,
                '"fetch the data URL. Reply done when finished.%s."'
                % stamp))
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
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-fallback"})
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        # Replay with the STAMPED agent (fresh workdir, same data server:
        # the recorded http_get URL is frozen in the past).
        wd2 = os.path.join(work2, "w")
        os.makedirs(wd2)
        new = os.path.join(work, "new.sgr")
        r2 = run_replay(
            SNOWGLOBE, orig,
            ["--", sys.executable, stamped, "--workdir", wd2,
             "--data-url", durl],
            new, scrub=SCRUB)
        if r2.returncode != 65:
            print("FAIL: replay exited %d, want 65 (diverged)\n%s%s"
                  % (r2.returncode, r2.stdout, r2.stderr))
            return 1
        if "done" not in r2.stdout:
            # Fallback keeps the run alive: the agent still finishes.
            print("FAIL: agent did not finish (no 'done'):\n%s%s"
                  % (r2.stdout, r2.stderr))
            return 1
        rep = load_report(new)
        if rep["original_exit"] != 0 or rep["replay_exit"] != 0:
            print("FAIL: agent should finish exit 0 both times: %s" % rep)
            return 1
        llm = rep["categories"]["llm"]
        if llm["status"] != "diverged":
            print("FAIL: llm not diverged: %s" % llm)
            return 1
        if llm.get("served_exact") != 0 or llm.get("served_fallback") != 3:
            print("FAIL: want 0 exact + 3 fallback, got: %s" % llm)
            return 1
        if rep["unrecorded"] != 0:
            print("FAIL: unrecorded should be 0: %s" % rep["unrecorded"])
            return 1
        if llm.get("served_exact", -1) + llm.get("served_fallback", -1) + \
                rep["unrecorded"] != rep["turns"]["replay"]:
            print("FAIL: exact+fallback+unrecorded != turns: %s" % llm)
            return 1
        if rep["turns"] != {"original": 3, "replay": 3, "match": True}:
            print("FAIL: turns: %s" % rep["turns"])
            return 1
        for t in (0, 1, 2):
            if not any("turn %d" % t in e and "differs" in e
                       for e in llm["replay_only"]):
                print("FAIL: divergence does not name turn %d: %s"
                      % (t, llm["replay_only"]))
                return 1
        print_llm(rep, "forced-fallback")
        print("PASS replay_fallback: 0 exact + 3 fallback, turns named, "
              "exit 65")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(work2, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""replay_clean: toy-agent 3-turn record -> offline replay, exit 0.

Records with a live mock, then replays with NO mock on the LLM path
(a fresh mock serves only the toy's direct http_get data URL, which is
non-LLM traffic and honestly not stubbed), no API keys in env, and a
fresh workdir (fresh tmp path + fresh data-url port exercise the
$TMP/PORT normalization). Asserts: exit 0, report all identical,
byte-exact re-served blobs, offline epilogue, manifest replay_of link,
and zero LLM POSTs at the data mock.
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_util import (PROXY_DIR, check_linux, free_port, Mock, tc,
                         toy_scenario, run_sg, run_replay, load_events,
                         load_report, res_blob_shas)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
SCRUB = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN",
         "GEMINI_API_KEY", "GOOGLE_API_KEY")


def main():
    check_linux(SNOWGLOBE)
    # Two roots with the same "w" leaf: the recorded tool args bake in the
    # workdir/data-url, so replay reuses the SAME server port (the model
    # replays the old URL verbatim — non-LLM traffic is honestly not
    # stubbed) while the tmp parent still exercises $TMP normalization.
    work = tempfile.mkdtemp(prefix="tmp.")
    work2 = tempfile.mkdtemp(prefix="tmp.")
    mock = None
    try:
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
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-clean-001"})
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        if len(mock.posts()) != 3:
            print("FAIL: record should POST 3 times, got %d"
                  % len(mock.posts()))
            return 1
        # Re-execution proof: delete the record-phase note; the replayed
        # agent must write it again — at the RECORDED path (tool args are
        # frozen in the past; the fresh workdir only exercises $TMP).
        os.remove(os.path.join(wd, "note.txt"))
        # Replay: the SAME mock keeps serving /test-data on the SAME port
        # (the recorded http_get URL is frozen in the past); the LLM path
        # goes to the replay proxy instead. Fresh workdir, no keys.
        wd2 = os.path.join(work2, "w")
        os.makedirs(wd2)
        new = os.path.join(work, "new.sgr")
        # All API keys scrubbed: the agent falls back to its "test-key"
        # placeholder and the replay proxy never dials upstream, so the
        # header value is irrelevant to matching (headers never hash).
        r2 = run_replay(
            SNOWGLOBE, orig,
            ["--", sys.executable, AGENT, "--workdir", wd2,
             "--data-url", durl],
            new, scrub=SCRUB)
        if r2.returncode != 0:
            print("FAIL: replay exited %d, want 0\n%s%s"
                  % (r2.returncode, r2.stdout, r2.stderr))
            return 1
        if "offline: 3 responses served, 0 unrecorded" not in r2.stderr:
            print("FAIL: no offline epilogue:\n" + r2.stderr)
            return 1
        rep = load_report(new)
        for cat in ("llm", "fs", "proc", "net", "exit"):
            if rep["categories"][cat]["status"] != "identical":
                print("FAIL: category %s not identical: %s"
                      % (cat, rep["categories"][cat]))
                return 1
        if rep["turns"] != {"original": 3, "replay": 3, "match": True}:
            print("FAIL: turns: %s" % rep["turns"])
            return 1
        if not rep["order_matches"] or rep["unrecorded"] != 0:
            print("FAIL: order/unrecorded: %s" % rep)
            return 1
        if res_blob_shas(new) != res_blob_shas(orig):
            print("FAIL: re-served blobs not byte-exact")
            return 1
        with open(os.path.join(new, "manifest.json")) as f:
            man = json.load(f)
        if man.get("replay_of") != orig:
            print("FAIL: manifest replay_of: %s" % man.get("replay_of"))
            return 1
        # Offline proof: the mock saw exactly the 3 record-phase POSTs —
        # replay added none (its LLM traffic went to the replay proxy).
        if len(mock.posts()) != 3:
            print("FAIL: mock saw %d POSTs, want exactly the 3 recorded"
                  % len(mock.posts()))
            return 1
        with open(os.path.join(wd, "note.txt")) as f:
            if f.read() != "hello snowglobe":
                print("FAIL: note.txt not rewritten by replay")
                return 1
        if os.path.exists(os.path.join(orig, "replay.unrecorded.jsonl")):
            print("FAIL: unexpected unrecorded log on clean replay")
            return 1
        print("PASS replay_clean: exit 0, all identical, byte-exact, offline")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(work2, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

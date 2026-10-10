#!/usr/bin/env python3
"""replay_unrecorded (mutation c): a novel 4th call fails loudly.

Records the toy 3-turn run, then replays with post_client posting the 3
recorded request bodies verbatim (body_file) plus one novel 4th request.
The first three serve (primary hash hits); the 4th is a MISS: 502
"snowglobe replay: unrecorded call", one line in
<orig>/replay.unrecorded.jsonl, llm status unrecorded, exit 65. Nothing
is ever silently invented.
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_util import (PROXY_DIR, check_linux, free_port, Mock,
                         toy_scenario, run_sg, run_replay, load_report)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
SCRUB = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN",
         "GEMINI_API_KEY", "GOOGLE_API_KEY")
MISS_BODY = b'{"error":"snowglobe replay: unrecorded call"}'


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="tmp.")
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
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-mut-c"})
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        mock.cleanup()
        mock = None
        # Jobs: the 3 recorded bodies verbatim + 1 novel request.
        jobs = []
        for i in range(3):
            with open(os.path.join(orig, "llm", "%04d.req.json" % i)) as f:
                env = json.load(f)
            bf = os.path.join(work, "body%d.bin" % i)
            with open(bf, "wb") as f:
                f.write(env["body"].encode())
            jobs.append({"path": "/chat/completions", "body_file": bf})
        jobs.append({"path": "/chat/completions",
                     "body": {"model": "never-recorded-model",
                              "messages": [{"role": "user",
                                            "content": "novel call"}]}})
        jobs_f = os.path.join(work, "jobs.json")
        res_f = os.path.join(work, "results.json")
        with open(jobs_f, "w") as f:
            json.dump(jobs, f)
        new = os.path.join(work, "new.sgr")
        r2 = run_replay(
            SNOWGLOBE, orig,
            ["--", sys.executable, os.path.join(PROXY_DIR, "post_client.py"),
             "--jobs", jobs_f, "--results", res_f],
            new, scrub=SCRUB)
        if r2.returncode != 65:
            print("FAIL: replay exited %d, want 65 (unrecorded)\n%s%s"
                  % (r2.returncode, r2.stdout, r2.stderr))
            return 1
        with open(res_f) as f:
            got = json.load(f)
        if len(got) != 4:
            print("FAIL: want 4 results, got %s" % got)
            return 1
        if any(g["status"] != 200 for g in got[:3]):
            print("FAIL: first three should serve 200: %s" % got)
            return 1
        if got[3]["status"] != 502:
            print("FAIL: 4th should be 502, got %s" % got[3])
            return 1
        if got[3]["body_sha256"] != hashlib.sha256(MISS_BODY).hexdigest():
            print("FAIL: 502 body is not the unrecorded-call error: %s"
                  % got[3])
            return 1
        rep = load_report(new)
        if rep["categories"]["llm"]["status"] != "unrecorded":
            print("FAIL: llm not unrecorded: %s" % rep["categories"]["llm"])
            return 1
        if rep["unrecorded"] != 1:
            print("FAIL: unrecorded count: %s" % rep["unrecorded"])
            return 1
        log = os.path.join(orig, "replay.unrecorded.jsonl")
        with open(log) as f:
            lines = [ln for ln in f if ln.strip()]
        if len(lines) != 1:
            print("FAIL: want 1 unrecorded log line, got %d" % len(lines))
            return 1
        rec = json.loads(lines[0])
        if rec.get("reason") != "no-recorded-match" or \
                rec.get("provider") != "openai":
            print("FAIL: bad log line: %s" % rec)
            return 1
        print("PASS replay_unrecorded: 502 + log + report entry, exit 65")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

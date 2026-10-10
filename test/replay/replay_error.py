#!/usr/bin/env python3
"""replay_error: failure parity counts as clean.

Phase 1: the committed claude-code-1-error fixture through
`snowglobe replay-proxy` — replay_client reposts all 12 recorded
envelopes verbatim; every response must serve byte-exact (1x recorded
502 probe + 11x401), offline epilogue, zero unrecorded.

Phase 2: a fresh 401 record -> replay. post_client makes one call
against a mock returning 401, then the shell exits 1 (emulating the
failed agent). Replay re-serves the 401 offline; the agent fails with
the same exit 1 -> report identical, "same failure", replay exits 0.
"""
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_util import (check_linux, free_port, Mock, run_sg,
                         run_replay, load_report, res_blob_shas)

SNOWGLOBE, MOCKLLM = sys.argv[1], sys.argv[2]
REPLAY_DIR = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.join(REPLAY_DIR, "replay_client.py")
FIXTURE = os.path.normpath(os.path.join(REPLAY_DIR, "..", "fixtures",
                                         "real", "claude-code-1-error"))
SCRUB = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN",
         "GEMINI_API_KEY", "GOOGLE_API_KEY",
         "AWS_SECRET_ACCESS_KEY", "GITHUB_TOKEN")


def start_replay_proxy(snowglobe, run):
    p = subprocess.Popen([snowglobe, "replay-proxy", run],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True)
    port = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < 30:
        if p.poll() is not None:
            raise RuntimeError("replay-proxy died: " + p.stderr.read())
        # The listen line carries the ephemeral port; poll stderr with a
        # deadline (the pipe is block-buffered nowhere: stderr is
        # unbuffered, one line is always available once printed).
        r, _, _ = select.select([p.stderr], [], [], 0.2)
        if r:
            line = p.stderr.readline()
            if "on 127.0.0.1:" in line:
                port = int(line.split("127.0.0.1:")[1].split()[0])
                break
    if port is None:
        p.kill()
        raise RuntimeError("replay-proxy never printed a port")
    return p, port


def phase_fixture():
    p, port = start_replay_proxy(SNOWGLOBE, FIXTURE)
    work = tempfile.mkdtemp(prefix="tmp.")
    try:
        res = os.path.join(work, "results.json")
        env = dict(os.environ)
        for k in SCRUB:
            env.pop(k, None)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["OPENAI_BASE_URL"] = "http://127.0.0.1:%d/openai/v1" % port
        env["ANTHROPIC_BASE_URL"] = "http://127.0.0.1:%d/anthropic" % port
        env["GEMINI_BASE_URL"] = "http://127.0.0.1:%d/gemini" % port
        # The fixture's agent exited 1; emulate the same failure.
        r = subprocess.run(
            [sys.executable, CLIENT, "--run", FIXTURE, "--results", res,
             "--exit-code", "1"],
            capture_output=True, text=True, env=env, timeout=120)
        if r.returncode != 1:
            print("FAIL: fixture client exited %d, want 1\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res) as f:
            got = json.load(f)
        if len(got) != 12:
            print("FAIL: want 12 re-served turns, got %d" % len(got))
            return 1
        want_status = [502] + [401] * 11
        if [g["status"] for g in got] != want_status:
            print("FAIL: statuses: %s" % [g["status"] for g in got])
            return 1
        # Byte-exact vs the committed blobs (re-chunking must be exact).
        # Exception: turn 0 is the HEAD probe — HTTP carries no body on
        # HEAD responses, so the original client saw an empty body too;
        # assert status parity + empty wire body there (the 62 stored
        # bytes are forensic, and the event keeps bytes:62 like the
        # original recording).
        import hashlib
        stored = res_blob_shas(FIXTURE)
        if got[0]["body_bytes"] != 0 or \
                got[0]["body_sha256"] != hashlib.sha256(b"").hexdigest():
            print("FAIL: HEAD probe should re-serve empty wire body: %s"
                  % got[0])
            return 1
        for g in got[1:]:
            if g["body_sha256"] != stored[g["id"]]:
                print("FAIL: turn %d not byte-exact" % g["id"])
                return 1
        p.send_signal(signal.SIGTERM)
        _, err = p.communicate(timeout=30)
        if "offline: 12 responses served, 0 unrecorded" not in err:
            print("FAIL: no offline epilogue:\n" + err)
            return 1
        print("PASS replay_error/phase1: 12 fixture turns byte-exact, offline")
        return 0
    finally:
        try:
            if p.poll() is None:
                p.kill()
                p.communicate(timeout=10)
        except Exception:
            pass
        shutil.rmtree(work, ignore_errors=True)


def phase_fresh():
    work = tempfile.mkdtemp(prefix="tmp.")
    mock = None
    try:
        port = free_port()
        err_body = {"error": {"message": "auth failed",
                              "type": "authentication_error"}}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port,
                    {"responses": [{"status": 401, "stream": False,
                                    "body": err_body}]})
        jobs = [{"path": "/chat/completions",
                 "body": {"model": "mock-model-1", "stream": False}}]
        jobs_f = os.path.join(work, "jobs.json")
        res_f = os.path.join(work, "results.json")
        with open(jobs_f, "w") as f:
            json.dump(jobs, f)
        from replay_util import PROXY_DIR
        post = os.path.join(PROXY_DIR, "post_client.py")
        # The shell exits 1 after the failed call (failed-agent shape).
        cmd = ["sh", "-c", "%s %s --jobs %s --results %s; exit 1"
               % (sys.executable, post, jobs_f, res_f)]
        orig = os.path.join(work, "orig.sgr")
        r = run_sg(SNOWGLOBE, ["--upstream=openai=" + mock.base, "--"] + cmd,
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-err-fresh"})
        if r.returncode != 1:
            print("FAIL: record exited %d, want 1\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        if "1 LLM turn (1 error)" not in r.stderr:
            print("FAIL: record epilogue:\n" + r.stderr)
            return 1
        mock.cleanup()
        mock = None
        new = os.path.join(work, "new.sgr")
        r2 = run_replay(SNOWGLOBE, orig, [], new, scrub=SCRUB)
        if r2.returncode != 0:
            print("FAIL: replay exited %d, want 0 (same failure)\n%s%s"
                  % (r2.returncode, r2.stdout, r2.stderr))
            return 1
        if "same failure as original (exit 1)" not in r2.stderr:
            print("FAIL: no same-failure line:\n" + r2.stderr)
            return 1
        rep = load_report(new)
        for cat in ("llm", "fs", "proc", "net", "exit"):
            if rep["categories"][cat]["status"] != "identical":
                print("FAIL: category %s not identical: %s"
                      % (cat, rep["categories"][cat]))
                return 1
        if rep["original_exit"] != 1 or rep["replay_exit"] != 1:
            print("FAIL: exits: %s" % rep)
            return 1
        print("PASS replay_error/phase2: same failure is clean, exit 0")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


def main():
    check_linux(SNOWGLOBE)
    if len(sys.argv) < 3:
        print("usage: replay_error.py <snowglobe> <mockllm>")
        return 2
    if not os.path.isdir(FIXTURE):
        skip("fixture missing: " + FIXTURE)
    rc = phase_fixture()
    if rc != 0:
        return rc
    return phase_fresh()


if __name__ == "__main__":
    sys.exit(main())

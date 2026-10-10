#!/usr/bin/env python3
"""replay_concurrent: 8 parallel streams stay byte-exact under replay.

Records conc_client (8 concurrent identical streaming POSTs, distinct
bodies per stream from the mock) via `run`, then replays with the same
client and no mock on the LLM path. Every re-served body must equal its
recorded blob byte-for-byte (multiset over 8 shas: cross-stream
contamination cannot hide), report identical, exit 0.
"""
import collections
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_util import (PROXY_DIR, check_linux, free_port, Mock, run_sg,
                         run_replay, load_events, load_report, sha_file)

SNOWGLOBE, MOCKLLM = sys.argv[1], sys.argv[2]
N = 8
SCRUB = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN",
         "GEMINI_API_KEY", "GOOGLE_API_KEY")


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="tmp.")
    mock = None
    try:
        port = free_port()
        scenario = {"responses": [
            {"status": 200, "stream": True,
             "chunks": ["data: {\"s\":%d,\"k\":%d}\n\n" % (j, i)
                        for i in range(10)]}
            for j in range(N)]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        client = os.path.join(PROXY_DIR, "conc_client.py")
        res = os.path.join(work, "conc.json")
        orig = os.path.join(work, "orig.sgr")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, client, "--results", res,
                    "--count", str(N)],
                   orig)
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        mock.cleanup()
        mock = None
        # Replay: same client, same results path (argv+paths identical so
        # the side-effect multisets compare equal), no upstream at all.
        new = os.path.join(work, "new.sgr")
        r2 = run_replay(
            SNOWGLOBE, orig,
            ["--", sys.executable, client, "--results", res,
             "--count", str(N)],
            new, scrub=SCRUB)
        if r2.returncode != 0:
            print("FAIL: replay exited %d, want 0\n%s%s"
                  % (r2.returncode, r2.stdout, r2.stderr))
            return 1
        with open(res) as f:
            got = json.load(f)
        if len(got) != N or any(g["status"] != 200 for g in got):
            print("FAIL: client results: %s" % got)
            return 1
        evs = load_events(new)
        resps = {e["id"]: e for e in evs if e.get("ev") == "llm.response"}
        if len(resps) != N:
            print("FAIL: want %d responses, got %d" % (N, len(resps)))
            return 1
        recorded = collections.Counter()
        for i, s in resps.items():
            if s["status"] != 200 or s["truncated"]:
                print("FAIL: bad response %s" % s)
                return 1
            recorded[sha_file(os.path.join(new, s["res"]))] += 1
        # New-run blobs must equal the ORIGINAAL blobs (byte-exact re-serve).
        oevs = load_events(orig)
        oresps = {e["id"]: e for e in oevs if e.get("ev") == "llm.response"}
        original = collections.Counter(
            sha_file(os.path.join(orig, s["res"])) for s in oresps.values())
        if recorded != original or any(v != 1 for v in recorded.values()):
            print("FAIL: replayed %s != recorded %s (want 8 distinct 1:1)"
                  % (recorded, original))
            return 1
        received = collections.Counter(g["body_sha256"] for g in got)
        if received != recorded:
            print("FAIL: per-thread received %s != stored %s"
                  % (received, recorded))
            return 1
        rep = load_report(new)
        for cat in ("llm", "fs", "proc", "net", "exit"):
            if rep["categories"][cat]["status"] != "identical":
                print("FAIL: category %s not identical: %s"
                      % (cat, rep["categories"][cat]))
                return 1
        print("PASS replay_concurrent: 8 streams byte-exact, exit 0")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Phase 4 Block 2: 8 concurrent streams under --backend=notify.

Same pairwise-distinct-body bijection as proxy_concurrent (stored <->
sent, received <-> stored, 8 distinct sha256), plus the notify shape
spot-checks that matter here: run.meta backend:notify, net.* outcome
events carry result_known:false and no ok key, zero decode_error.
Backend preflight skips (69) with a reason.
"""
import collections
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import (PROXY_DIR, check_linux, free_port, Mock, load_events,
                         llm_pairs, run_sg, sha_file, skip)

SNOWGLOBE, MOCKLLM = sys.argv[1], sys.argv[2]
N = 8


def preflight():
    work = tempfile.mkdtemp(prefix="sg-nconc-pre-")
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
    work = tempfile.mkdtemp(prefix="sg-nconc-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        scenario = {"responses": [
            {"status": 200, "stream": True,
             "chunks": ["data: {\"s\":%d,\"k\":%d}\n\n" % (j, i)
                        for i in range(10)]}
            for j in range(N)]}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        res = os.path.join(work, "conc.json")
        r = run_sg(SNOWGLOBE,
                   ["--backend=notify", "--upstream=openai=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "conc_client.py"),
                    "--results", res, "--count", str(N)],
                   out, timeout=180)
        if r.returncode != 0:
            print("FAIL: snowglobe run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res) as f:
            got = json.load(f)
        if len(got) != N or any(g["status"] != 200 for g in got):
            print("FAIL: client results: %s" % got)
            return 1
        evs = load_events(out)
        reqs, resps = llm_pairs(evs)
        if len(reqs) != N or len(resps) != N:
            print("FAIL: want %d pairs, got %d req %d res"
                  % (N, len(reqs), len(resps)))
            return 1
        stored = collections.Counter()
        for i, s in resps.items():
            if s["status"] != 200 or s["truncated"]:
                print("FAIL: bad response %s" % s)
                return 1
            stored[sha_file(os.path.join(out, s["res"]))] += 1
        sent_hashes = collections.Counter()
        for p in glob.glob(os.path.join(mock.sent, "res-*.bin")):
            sent_hashes[sha_file(p)] += 1
        if stored != sent_hashes or any(v != 1 for v in stored.values()):
            print("FAIL: stored %s != sent %s (want 8 distinct 1:1)"
                  % (stored, sent_hashes))
            return 1
        received = collections.Counter(g["body_sha256"] for g in got)
        if received != stored:
            print("FAIL: per-thread received %s != stored %s" % (received, stored))
            return 1
        # Notify shape spot-checks.
        metas = [e for e in evs if e.get("ev") == "run.meta"]
        if len(metas) != 1 or metas[0].get("backend") != "notify":
            print("FAIL: run.meta backend != notify")
            return 1
        if any(e.get("ev") == "trace.decode_error" for e in evs):
            print("FAIL: trace.decode_error present")
            return 1
        for e in evs:
            if e.get("ev") in ("net.connect", "net.sendto", "net.bind"):
                if e.get("result_known") is not False or "ok" in e:
                    print("FAIL: bad net shape: %s" % e)
                    return 1
        print("PASS notify_concurrent: %d streams pairwise byte-exact "
              "under notify" % N)
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

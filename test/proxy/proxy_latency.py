#!/usr/bin/env python3
"""9(b): streaming latency — the mock delays its last SSE chunk 2 s; the
client must see the first chunk long before that (zero added buffering),
and median(TTFB_proxy − TTFB_direct) over 20 paired runs must be under
budget: 5 ms on plain builds, 25 ms under ASan/TSan (SG_LAT_BUDGET_S,
set by test/CMakeLists from the sanitize flags — instrumentation slows
the proxy ~5x; debug measures the product number, sanitizers bound it).
One retry on budget-miss only: parallel-load noise can spike a single
median, so a miss reruns once and both medians print; a real regression
fails twice.
"""
import json
import os
import shutil
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import (PROXY_DIR, check_linux, free_port, Mock, run_sg)

SNOWGLOBE, MOCKLLM = sys.argv[1], sys.argv[2]
ITERS = 20
BUDGET = float(os.environ.get("SG_LAT_BUDGET_S", "0.005"))


def stream_body():
    return {"model": "mock-model-1", "stream": True}


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-lat-")
    out = os.path.join(work, "run.sgr")
    mock = None
    try:
        port = free_port()
        quick = {"status": 200, "stream": True,
                 "chunks": ["data: {\"i\":1}\n\n", "data: {\"i\":2}\n\n"]}
        delayed = {"status": 200, "stream": True,
                   "chunks": ["data: {\"i\":1}\n\n", "data: {\"i\":2}\n\n"],
                   "delay_last_ms": 2000}
        # Two full client runs' worth: the retry below consumes the second
        # half from the same FIFO mock.
        one_run = [delayed] + [quick] * (2 * ITERS)
        scenario = {"responses": one_run + one_run}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)

        def one_client_run(tag, res_path, sgr):
            r = run_sg(SNOWGLOBE,
                       ["--upstream=openai=" + mock.base, "--",
                        sys.executable, os.path.join(PROXY_DIR, "lat_client.py"),
                        "--mock-url", mock.base, "--results", res_path,
                        "--iters", str(ITERS)],
                       sgr, timeout=300)
            if r.returncode != 0:
                print("FAIL: snowglobe run %s exited %d\n%s%s"
                      % (tag, r.returncode, r.stdout, r.stderr))
                return None
            with open(res_path) as f:
                m = json.load(f)
            if not m["first"] < 1.5:
                print("FAIL: %s first chunk took %.3fs (last sent at ~2s): "
                      "proxy is buffering" % (tag, m["first"]))
                return None
            diffs = [p - d for p, d in zip(m["proxy"], m["direct"])]
            return m["first"], statistics.median(diffs)

        res = os.path.join(work, "lat.json")
        got = one_client_run("run1", res, out)
        if got is None:
            return 1
        first, med = got
        if not med < BUDGET:
            # Retry once: parallel-load noise (ctest -j16) can spike one
            # 20-sample median; a real regression fails twice. Both
            # medians are reported, so a flake can never hide silently.
            print("note: run1 median %.4fs >= budget %.4fs, retrying once"
                  % (med, BUDGET))
            res2 = os.path.join(work, "lat2.json")
            out2 = os.path.join(work, "run2.sgr")
            got2 = one_client_run("run2", res2, out2)
            if got2 is None:
                return 1
            first2, med2 = got2
            if not med2 < BUDGET:
                print("FAIL: run1 median %.4fs and run2 median %.4fs both "
                      ">= budget %.4fs" % (med, med2, BUDGET))
                return 1
            print("note: run2 median %.4fs < budget (run1 flaked)" % med2)
            first, med = first2, med2
        print("PASS latency: first=%.3fs (<1.5s), median TTFB delta=%.4fs "
              "(budget %.4fs, %d paired runs)" % (first, med, BUDGET, ITERS))
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

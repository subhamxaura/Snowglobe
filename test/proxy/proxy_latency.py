#!/usr/bin/env python3
"""9(b): streaming latency — the mock delays its last SSE chunk 2 s; the
client must see the first chunk long before that (zero added buffering),
and median(TTFB_proxy − TTFB_direct) over 20 paired runs must be under
budget: 5 ms on plain builds, 25 ms under ASan/TSan (SG_LAT_BUDGET_S,
set by test/CMakeLists from the sanitize flags — instrumentation slows
the proxy ~5x; debug measures the product number, sanitizers bound it).
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
        scenario = {"responses": [delayed] + [quick] * (2 * ITERS)}
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port, scenario)
        res = os.path.join(work, "lat.json")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, os.path.join(PROXY_DIR, "lat_client.py"),
                    "--mock-url", mock.base, "--results", res,
                    "--iters", str(ITERS)],
                   out, timeout=300)
        if r.returncode != 0:
            print("FAIL: lat_client exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        with open(res) as f:
            m = json.load(f)
        if not m["first"] < 1.5:
            print("FAIL: first chunk took %.3fs (last sent at ~2s): "
                  "proxy is buffering" % m["first"])
            return 1
        diffs = [p - d for p, d in zip(m["proxy"], m["direct"])]
        med = statistics.median(diffs)
        if not med < BUDGET:
            print("FAIL: median TTFB delta %.4fs >= budget %.4fs (diffs=%s)"
                  % (med, BUDGET, ["%.4f" % x for x in diffs]))
            return 1
        print("PASS latency: first=%.3fs (<1.5s), median TTFB delta=%.4fs "
              "(budget %.4fs, %d paired runs)" % (m["first"], med, BUDGET,
                                                  ITERS))
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

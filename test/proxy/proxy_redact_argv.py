#!/usr/bin/env python3
"""9(g): argv redaction end-to-end — the secret travels only in argv, so the
recorded proc.exec / run.meta must read REDACTED and a byte grep over the
whole run dir must be empty.
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sgtest_util import check_linux, grep_bytes, load_events, run_sg

SNOWGLOBE = sys.argv[1]
SECRET = "sk-test-argv-999"


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-redact-")
    out = os.path.join(work, "run.sgr")
    try:
        r = run_sg(SNOWGLOBE,
                   ["--", "sh", "-c", "echo Bearer %s >/dev/null" % SECRET],
                   out, timeout=120)
        if r.returncode != 0:
            print("FAIL: run exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        evs = load_events(out)
        execs = [e for e in evs if e.get("ev") == "proc.exec"]
        sh_exec = [e for e in execs
                   if e.get("path", "").endswith("/sh") and
                   "Bearer" in " ".join(e.get("argv", []))]
        if not sh_exec:
            print("FAIL: no sh proc.exec with Bearer argv")
            return 1
        for e in execs:
            for a in e.get("argv", []):
                if SECRET in a:
                    print("FAIL: secret in proc.exec argv: %s" % e)
                    return 1
        if not any("REDACTED" in a for e in sh_exec for a in e["argv"]):
            print("FAIL: no REDACTED marker in sh argv: %s" % sh_exec)
            return 1
        with open(os.path.join(out, "manifest.json")) as f:
            m = json.load(f)
        if any(SECRET in a for a in m["cmd"]):
            print("FAIL: secret in manifest cmd: %s" % m["cmd"])
            return 1
        hits = grep_bytes(out, SECRET)
        if hits:
            print("FAIL: secret bytes present in: %s" % hits)
            return 1
        print("PASS redact_argv: argv + manifest redacted, run dir clean")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

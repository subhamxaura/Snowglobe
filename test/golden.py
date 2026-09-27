#!/usr/bin/env python3
"""Run one scenario under snowglobe and compare normalized events to goldens.

Usage: golden.py <snowglobe> <scenario-dir> <repo> <helpers-dir> [--update]
<helpers-dir> holds compiled C scenario helpers; run.sh reads it as
SG_HELPERS (env is never recorded, helper paths normalise to $HELPERS).
Regeneration also honors SNOWGLOBE_UPDATE_GOLDENS=1 (for `ctest -R golden`).
Regenerating goldens additionally requires a CHANGELOG line (process rule,
see test/fixtures/scenarios/README.md); the flag alone is not enough.
"""
import difflib
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

NORM_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "normalize.py")


def main():
    args = sys.argv[1:]
    update = False
    if "--update" in args:
        update = True
        args.remove("--update")
    if os.environ.get("SNOWGLOBE_UPDATE_GOLDENS") == "1":
        update = True
    sg, scn, repo = args[0], args[1], args[2]
    helpers = args[3] if len(args) > 3 else ""
    scn = os.path.abspath(scn)  # run.sh must be absolute: the tracee
    repo = os.path.abspath(repo)  # execs it with cwd=scn, so relative
    name = os.path.basename(scn.rstrip("/"))  # paths would 404 (exit 127)
    work = tempfile.mkdtemp(prefix="sg-golden-")
    out = os.path.join(work, "run.sgr")
    run_sh = os.path.join(scn, "run.sh")
    try:
        env = dict(os.environ, SG_HELPERS=helpers)
        r = subprocess.run(
            [sg, "run", "--out=" + out, "--", run_sh],
            cwd=scn,
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
        )
        if r.returncode != 0:
            print("FAIL %s: scenario exited %d\n%s%s" % (name, r.returncode, r.stdout, r.stderr))
            return 1
        cmd = [sys.executable, NORM_PY, os.path.join(out, "events.jsonl"), "--repo", repo]
        if helpers:
            cmd += ["--helpers", helpers]
        n = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if n.returncode != 0:
            print("FAIL %s: normalize failed\n%s" % (name, n.stderr))
            return 1
        actual = [l for l in n.stdout.splitlines() if l.strip()]
    finally:
        shutil.rmtree(work, ignore_errors=True)
    golden_path = os.path.join(scn, "expected.jsonl")
    if update:
        with open(golden_path, "w") as f:
            f.write("\n".join(actual) + "\n")
        print("UPDATED %s: %d events (remember the CHANGELOG line)" % (name, len(actual)))
        return 0
    if not os.path.exists(golden_path):
        print("FAIL %s: no golden; regenerate with --update" % name)
        return 1
    with open(golden_path) as f:
        expected = [l.rstrip("\n") for l in f if l.strip()]
    if expected != actual:
        print(
            "FAIL %s: golden mismatch (%d expected vs %d actual); regenerate with --update only if the change is intended"
            % (name, len(expected), len(actual))
        )
        for line in difflib.unified_diff(expected, actual, "expected", "actual", lineterm=""):
            print(line)
        return 1
    print("PASS %s: %d events" % (name, len(actual)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

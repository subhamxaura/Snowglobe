#!/usr/bin/env python3
"""Phase 2 Block 1: --isolate containment — repo writes land in the
overlay upper (host clean), /etc writes copy up (host clean), host pids
invisible from the empty /proc, manifest + run.meta carry the isolate
record, bad projects exit 64. Stdlib only.
Usage: isolate_basic.py <snowglobe-bin>.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "proxy"))
from sgtest_util import check_linux, load_events, skip  # noqa: E402

SNOWGLOBE = sys.argv[1]


def run_sg_iso(args, out, flags=(), cwd=None, timeout=120):
    return subprocess.run(
        [SNOWGLOBE, "run", "--out=" + out, "--isolate", *flags, "--"] + args,
        capture_output=True, text=True, cwd=cwd, timeout=timeout)


def try_isolate():
    work = tempfile.mkdtemp(prefix="sg-iso-probe-")
    try:
        r = run_sg_iso(["/bin/true"], os.path.join(work, "p.sgr"))
        if r.returncode == 69:
            skip("no --isolate capability here: " + r.stderr.strip()[-200:])
        if r.returncode != 0:
            print("FAIL: isolate smoke exited %d:\n%s" % (r.returncode, r.stderr))
            sys.exit(1)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def manifest(out):
    with open(os.path.join(out, "manifest.json")) as f:
        return json.load(f)


def main():
    check_linux(SNOWGLOBE)
    try_isolate()
    work = tempfile.mkdtemp(prefix="sg-iso-")
    ok = True
    try:
        # Bad projects stay EX_USAGE (no tracing attempted).
        out = os.path.join(work, "bad.sgr")
        r = subprocess.run(
            [SNOWGLOBE, "run", "--out=" + out, "--isolate",
             "--project=/nonexistent-sg-proj", "--", "/bin/true"],
            capture_output=True, text=True, timeout=60)
        if r.returncode != 64:
            print("FAIL: bad project must exit 64, got %d:\n%s"
                  % (r.returncode, r.stderr))
            ok = False
        r = subprocess.run(
            [SNOWGLOBE, "run", "--out=" + out, "--isolate",
             "--project=/", "--", "/bin/true"],
            capture_output=True, text=True, timeout=60)
        if r.returncode != 64:
            print("FAIL: project=/ must exit 64, got %d" % r.returncode)
            ok = False

        # (b) repo write: recorded, host clean, upper copy present.
        # The project MUST be passed explicitly: the default (cwd) may sit
        # under a shadowed tree and the write would land elsewhere.
        repo = os.path.join(work, "repo")
        os.makedirs(repo)
        out = os.path.join(work, "b.sgr")
        r = run_sg_iso(["sh", "-c", "echo repo-data > %s/greet.txt" % repo],
                       out, flags=["--project=" + repo])
        if r.returncode != 0:
            print("FAIL: repo write run exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        host_file = os.path.join(repo, "greet.txt")
        upper_file = os.path.join(out, "overlay", "upper", "greet.txt")
        if os.path.exists(host_file):
            print("FAIL: repo write leaked to host: " + host_file)
            ok = False
        if not os.path.exists(upper_file):
            print("FAIL: no upper copy-up: " + upper_file)
            ok = False
        else:
            with open(upper_file) as f:
                if f.read() != "repo-data\n":
                    print("FAIL: upper content wrong")
                    ok = False
        evs = load_events(out)
        if not any(e.get("ev") == "fs.open" and e.get("path") == host_file
                   and e.get("write") is True for e in evs):
            print("FAIL: no fs.open(write) for the repo write")
            ok = False
        metas = [e for e in evs if e.get("ev") == "run.meta"]
        if len(metas) != 1 or metas[0].get("isolate") is not True:
            print("FAIL: run.meta isolate record missing: %s" % metas)
            ok = False
        m = manifest(out)
        iso = m.get("isolate", {})
        if iso.get("on") is not True or iso.get("upper") != "overlay/upper":
            print("FAIL: manifest isolate record wrong: %s" % iso)
            ok = False
        if sorted(iso.get("features", [])) != ["mount", "overlay", "pid", "userns"]:
            print("FAIL: manifest isolate features wrong: %s" % iso)
            ok = False

        # (c) /etc write: succeeds in-overlay, host untouched.
        probe = "snowglobe-probe-%d" % os.getpid()
        out = os.path.join(work, "c.sgr")
        r = run_sg_iso(["sh", "-c", "echo etc-data > /etc/" + probe], out)
        if r.returncode != 0:
            print("FAIL: /etc write run exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        if os.path.exists("/etc/" + probe):
            print("FAIL: /etc write leaked to host")
            ok = False
        with open(os.path.join(out, "overlay", "etc-upper", probe)) as f:
            if f.read() != "etc-data\n":
                print("FAIL: etc-upper content wrong")
                ok = False

        # (d) host pids invisible from the empty /proc.
        out = os.path.join(work, "d.sgr")
        r = run_sg_iso(["sh", "-c", "ls /proc"], out)
        if r.returncode != 0:
            print("FAIL: ls /proc exited %d" % r.returncode)
            ok = False
        if str(os.getpid()) in (r.stdout or "").split():
            print("FAIL: host pid visible inside --isolate:\n%s" % r.stdout)
            ok = False

        print("PASS isolate_basic" if ok else "FAIL isolate_basic")
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

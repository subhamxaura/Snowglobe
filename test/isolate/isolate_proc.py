#!/usr/bin/env python3
"""Issue #4 / ADR-0012: --proc=host rbinds host /proc read-only.

Default: /proc/self/maps is unreadable (empty tmpfs), zero numeric
/proc entries, run.meta has no proc key, manifest says empty.
With --proc=host: maps readable with mapping lines, host pids visible
(/proc/1/cmdline opens, >=5 numeric entries), run.meta + manifest say
host, and every run prints the tradeoff warning. Flag misuse (bad
value, or --proc without --isolate) is a loud usage error. Stdlib only.
Usage: isolate_proc.py <snowglobe-bin>.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "proxy"))
from sgtest_util import check_linux, load_events, skip  # noqa: E402

SNOWGLOBE = sys.argv[1]
READER = ("import os;"
          "maps=open('/proc/self/maps').read();"
          "print('maps=%d' % len(maps));"
          "ds=[d for d in os.listdir('/proc') if d.isdigit()];"
          "print('pids=%d' % len(ds));"
          "one=open('/proc/1/cmdline','rb').read();"
          "print('init=%d' % len(one));"
          "assert len(ds) >= 5 and len(maps) > 0")


def run_iso(args, out, flags=(), timeout=120):
    return subprocess.run(
        [SNOWGLOBE, "run", "--out=" + out, "--isolate", *flags, "--"] + args,
        capture_output=True, text=True, timeout=timeout)


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-proc-")
    try:
        probe = os.path.join(work, "probe.sgr")
        r = run_iso(["/bin/true"], probe)
        if r.returncode == 69:
            skip("no --isolate capability here: " + r.stderr.strip()[-200:])
        if r.returncode != 0:
            print("FAIL: isolate smoke exited %d:\n%s" % (r.returncode, r.stderr))
            return 1
        proj = os.path.join(work, "proj")
        os.makedirs(proj)
        ok = True

        def check(cond, msg):
            nonlocal ok
            if not cond:
                print("FAIL: " + msg)
                ok = False

        # Misuse is loud usage error, never a silent default.
        r = run_iso(["/bin/true"], os.path.join(work, "x.sgr"),
                    ["--project=" + proj, "--proc=bogus"])
        check(r.returncode == 64 and "only accepts host" in r.stderr,
              "--proc=bogus must exit 64 loudly, got %d:\n%s"
              % (r.returncode, r.stderr[-300:]))
        r = subprocess.run(
            [SNOWGLOBE, "run", "--out=" + os.path.join(work, "y.sgr"),
             "--proc=host", "--", "/bin/true"],
            capture_output=True, text=True, timeout=60)
        check(r.returncode == 64 and "--isolate" in r.stderr,
              "--proc without --isolate must exit 64, got %d:\n%s"
              % (r.returncode, r.stderr[-300:]))

        # Default: empty /proc (mechanism + records).
        out = os.path.join(work, "empty.sgr")
        r = run_iso(["python3", "-c", READER], out, ["--project=" + proj])
        check(r.returncode != 0,
              "default isolate should fail the maps read, got %d" % r.returncode)
        with open(os.path.join(out, "manifest.json")) as f:
            man = json.load(f)
        check(man.get("isolate", {}).get("proc") == "empty",
              "manifest isolate.proc should be empty: %s" % man.get("isolate"))
        metas = [e for e in load_events(out) if e.get("ev") == "run.meta"]
        check(len(metas) == 1 and "proc" not in metas[0],
              "default run.meta must not carry proc")
        check("--proc=host" not in r.stderr,
              "default run must not print the host warning")

        # --proc=host: readable maps, visible host pids, records, warning.
        out = os.path.join(work, "host.sgr")
        r = run_iso(["python3", "-c", READER], out,
                    ["--project=" + proj, "--proc=host"])
        check(r.returncode == 0,
              "--proc=host reader exited %d:\n%s" % (r.returncode, r.stderr[-500:]))
        check("pids=" in r.stdout and "maps=" in r.stdout,
              "reader proof lines missing:\n%s" % r.stdout[-300:])
        with open(os.path.join(out, "manifest.json")) as f:
            man = json.load(f)
        check(man.get("isolate", {}).get("proc") == "host",
              "manifest isolate.proc should be host: %s" % man.get("isolate"))
        metas = [e for e in load_events(out) if e.get("ev") == "run.meta"]
        check(len(metas) == 1 and metas[0].get("proc") == "host",
              "run.meta proc should be host")
        check("--proc=host" in r.stderr and "sees host pids" in r.stderr,
              "every --proc=host run must warn:\n%s" % r.stderr[-300:])

        # doctor names the tradeoff.
        r = subprocess.run([SNOWGLOBE, "doctor", "--isolate"],
                           capture_output=True, text=True, timeout=120)
        check("proc-mode" in r.stdout and "sees host pids" in r.stdout,
              "doctor lacks the proc-mode tradeoff row:\n%s" % r.stdout)

        print("PASS isolate_proc" if ok else "FAIL isolate_proc")
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

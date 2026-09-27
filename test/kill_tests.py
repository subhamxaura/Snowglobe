#!/usr/bin/env python3
"""Automated kill tests (Phase 1A Block 2, items a/b/c).

Usage: kill_tests.py <snowglobe> <repo> <a|b|c>
All temp dirs print on failure. Orphan scans read /proc directly (no pgreg
dependency). Every test has internal timeouts; CTest adds TIMEOUT 120.

(a) SIGKILL snowglobe while tracing `sh -c 'sleep 100 & sleep 100 & wait'`:
    no sleep survives within 2 s.
(b) SIGKILL the root child: snowglobe exits 137 with a finalised manifest.
(c) SIGTERM snowglobe: exits 143, manifest finalised, no orphans.
"""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

SG = sys.argv[1]
REPO = sys.argv[2]
WHICH = sys.argv[3]

# Fixed exec search path: /usr/bin/sleep is the first hit everywhere, so
# proc.exec paths are exact and deterministic across machines.
ENV = dict(os.environ, PATH="/usr/bin:/bin")


def fail(msg):
    print("FAIL kill_%s: %s" % (WHICH, msg))
    return 1


def sleep_procs():
    """Pids whose argv[0] basename is exactly 'sleep' (excludes supervisors)."""
    found = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                parts = f.read().split(b"\0")
        except OSError:
            continue
        if parts and os.path.basename(parts[0].decode(errors="replace")) == "sleep":
            found.append(int(pid))
    return found


def wait_no_sleep(deadline_s):
    """True once no sleep procs remain (checks every 0.1 s)."""
    end = time.time() + deadline_s
    while time.time() < end:
        if not sleep_procs():
            return True
        time.sleep(0.1)
    return not sleep_procs()


def wait_events(path, pred, timeout_s=15):
    """Wait until events.jsonl contains an event matching pred; return events."""
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            with open(path) as f:
                evs = [json.loads(l) for l in f if l.strip()]
        except (OSError, ValueError):
            evs = []
        if any(pred(e) for e in evs):
            return evs
        time.sleep(0.1)
    return evs


def manifest(out):
    with open(os.path.join(out, "manifest.json")) as f:
        return json.load(f)


def test_a():
    work = tempfile.mkdtemp(prefix="sg-kill-a-")
    out = os.path.join(work, "run.sgr")
    print("work: " + work)
    p = subprocess.Popen(
        [SG, "run", "--out=" + out, "--", "sh", "-c", "sleep 100 & sleep 100 & wait"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=ENV,
    )
    try:
        evs = wait_events(
            os.path.join(out, "events.jsonl"),
            lambda e: e.get("ev") == "proc.exec" and e.get("path") == "/usr/bin/sleep",
        )
        if not any(e.get("ev") == "proc.exec" for e in evs):
            return fail("root never execed sleep")
        p.kill()  # SIGKILL the supervisor
        p.wait(timeout=15)
        if wait_no_sleep(2.0):
            print("PASS kill_a: supervisor SIGKILLed, no sleep survives")
            return 0
        return fail("sleep survivors: %r" % sleep_procs())
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_b():
    work = tempfile.mkdtemp(prefix="sg-kill-b-")
    out = os.path.join(work, "run.sgr")
    print("work: " + work)
    p = subprocess.Popen(
        [SG, "run", "--out=" + out, "--", "sleep", "100"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=ENV,
    )
    try:
        evs = wait_events(
            os.path.join(out, "events.jsonl"),
            lambda e: e.get("ev") == "proc.start" and e.get("root") is True,
        )
        roots = [e["pid"] for e in evs if e.get("ev") == "proc.start" and e.get("root") is True]
        if not roots:
            return fail("no root proc.start observed")
        os.kill(roots[0], signal.SIGKILL)
        try:
            rc = p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            return fail("snowglobe did not exit after root SIGKILL")
        if rc != 137:
            return fail("exit %d, want 137" % rc)
        m = manifest(out)
        if m.get("finished") is None or not m.get("event_count"):
            return fail("manifest not finalised: %r" % m)
        print("PASS kill_b: exit 137, manifest finalised (%d events)" % m["event_count"])
        return 0
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_c():
    work = tempfile.mkdtemp(prefix="sg-kill-c-")
    out = os.path.join(work, "run.sgr")
    print("work: " + work)
    p = subprocess.Popen(
        [SG, "run", "--out=" + out, "--", "sleep", "100"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=ENV,
    )
    try:
        evs = wait_events(
            os.path.join(out, "events.jsonl"),
            lambda e: e.get("ev") == "proc.exec" and e.get("path") == "/usr/bin/sleep",
        )
        if not any(e.get("ev") == "proc.exec" for e in evs):
            return fail("root never execed sleep")
        p.terminate()  # SIGTERM the supervisor
        try:
            rc = p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            return fail("snowglobe did not exit after SIGTERM")
        if rc != 143:
            return fail("exit %d, want 143" % rc)
        m = manifest(out)
        if m.get("finished") is None:
            return fail("manifest not finalised")
        if not wait_no_sleep(2.0):
            return fail("orphan sleep survivors: %r" % sleep_procs())
        print("PASS kill_c: exit 143, manifest finalised, no orphans")
        return 0
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


if __name__ == "__main__":
    sys.exit({"a": test_a, "b": test_b, "c": test_c}[WHICH]())

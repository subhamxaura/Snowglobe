#!/usr/bin/env python3
"""Phase 4 Block 2: kill tests under --backend=notify (items a/b/c/d).

Usage: notify_kill.py <snowglobe> <repo> <a|b|c|d>
All temp dirs print on failure. Orphan scans read /proc directly. Every
test has internal timeouts; CTest adds TIMEOUT 120. Backend preflight:
exit 69 (no user-notify) skips with a reason, never fails.

(a) Supervisor SIGKILL with a fifo-blocked tracee: `cat fifo` blocks in
    kernel open() AFTER its notification was answered (the fs.open event
    proves it). Supervisor death must kill it via EXITKILL — an
    unanswered-or-parked notification must never wedge a child. No
    sleep/cat survivors within 2 s.
(b) SIGKILL the root child: snowglobe exits 137, manifest finalised.
(c) SIGTERM snowglobe: exits 143, manifest finalised, no orphans.
(d) Supervisor SIGKILL during a trapped-syscall storm loop (constant
    exec+open notifications in flight, unique binary path): no storm
    survivors within 2 s. This is the unanswered-notifications proof —
    at kill time some tracee is typically parked inside a notification.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SG = sys.argv[1]
REPO = sys.argv[2]
WHICH = sys.argv[3]

ENV = dict(os.environ, PATH="/usr/bin:/bin")


def fail(msg):
    print("FAIL notify_kill_%s: %s" % (WHICH, msg))
    return 1


def preflight():
    work = tempfile.mkdtemp(prefix="sg-nkill-pre-")
    try:
        r = subprocess.run(
            [SG, "run", "--out=" + os.path.join(work, "p.sgr"),
             "--backend=notify", "--", "/bin/true"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=ENV, timeout=60)
        if r.returncode == 69:
            print("SKIP notify_kill_%s: kernel lacks seccomp user-notify" % WHICH)
            sys.exit(0)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def procs_named(name):
    """Pids whose argv[0] basename is exactly name."""
    found = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                parts = f.read().split(b"\0")
        except OSError:
            continue
        if parts and os.path.basename(parts[0].decode(errors="replace")) == name:
            found.append(int(pid))
    return found


def wait_none(names, deadline_s):
    end = time.time() + deadline_s
    while time.time() < end:
        if not any(procs_named(n) for n in names):
            return True
        time.sleep(0.1)
    return not any(procs_named(n) for n in names)


def wait_events(path, pred, timeout_s=15):
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
    work = tempfile.mkdtemp(prefix="sg-nkill-a-")
    out = os.path.join(work, "run.sgr")
    print("work: " + work)
    p = subprocess.Popen(
        [SG, "run", "--out=" + out, "--backend=notify", "--",
         "sh", "-c", "D=$(mktemp -d); mkfifo $D/f; sleep 100 & cat $D/f & wait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
    try:
        # The fifo fs.open event proves cat's notification was ANSWERED
        # and cat proceeded into the blocking kernel open.
        evs = wait_events(
            os.path.join(out, "events.jsonl"),
            lambda e: e.get("ev") == "fs.open" and
            e.get("path", "").endswith("/f") and e.get("result_known") is False)
        if not any(e.get("ev") == "fs.open" and e.get("path", "").endswith("/f")
                   for e in evs):
            return fail("cat never reached the fifo open")
        time.sleep(1.0)  # let cat park inside the blocking open
        p.kill()  # SIGKILL the supervisor
        p.wait(timeout=15)
        if wait_none(("sleep", "cat"), 2.0):
            print("PASS notify_kill_a: supervisor SIGKILLed, fifo-blocked "
                  "tracee + sleep reaped, none wedged")
            return 0
        left = {n: procs_named(n) for n in ("sleep", "cat")}
        return fail("stranded survivors: %r" % left)
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_b():
    work = tempfile.mkdtemp(prefix="sg-nkill-b-")
    out = os.path.join(work, "run.sgr")
    print("work: " + work)
    p = subprocess.Popen(
        [SG, "run", "--out=" + out, "--backend=notify", "--", "sleep", "100"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
    try:
        evs = wait_events(
            os.path.join(out, "events.jsonl"),
            lambda e: e.get("ev") == "proc.start" and e.get("root") is True)
        roots = [e["pid"] for e in evs
                 if e.get("ev") == "proc.start" and e.get("root") is True]
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
        print("PASS notify_kill_b: exit 137, manifest finalised (%d events)"
              % m["event_count"])
        return 0
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_c():
    work = tempfile.mkdtemp(prefix="sg-nkill-c-")
    out = os.path.join(work, "run.sgr")
    print("work: " + work)
    p = subprocess.Popen(
        [SG, "run", "--out=" + out, "--backend=notify", "--", "sleep", "100"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
    try:
        evs = wait_events(
            os.path.join(out, "events.jsonl"),
            lambda e: e.get("ev") == "proc.exec" and e.get("path") == "/usr/bin/sleep")
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
        if not wait_none(("sleep",), 2.0):
            return fail("orphan sleep survivors: %r" % procs_named("sleep"))
        print("PASS notify_kill_c: exit 143, manifest finalised, no orphans")
        return 0
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_d():
    work = tempfile.mkdtemp(prefix="sg-nkill-d-")
    out = os.path.join(work, "run.sgr")
    print("work: " + work)
    # Unique binary path so the survivor scan cannot hit system cats.
    storm = os.path.join(work, "stormcat")
    shutil.copy("/bin/cat", storm)
    p = subprocess.Popen(
        [SG, "run", "--out=" + out, "--backend=notify", "--",
         "sh", "-c", "while true; do %s /etc/hostname > /dev/null; done" % storm],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
    try:
        # Wait for a real storm (several execs), not a single exec: at
        # kill time some tracee is typically parked inside a notification.
        n_exec = 0
        end = time.time() + 15
        while time.time() < end:
            try:
                with open(os.path.join(out, "events.jsonl")) as f:
                    evs = [json.loads(l) for l in f if l.strip()]
            except (OSError, ValueError):
                evs = []
            n_exec = sum(1 for e in evs
                         if e.get("ev") == "proc.exec" and e.get("path") == storm)
            if n_exec >= 5:
                break
            time.sleep(0.1)
        if n_exec < 5:
            return fail("storm never started (%d execs)" % n_exec)
        p.kill()  # SIGKILL the supervisor mid-notification-stream
        p.wait(timeout=15)
        if wait_none(("stormcat",), 2.0):
            print("PASS notify_kill_d: supervisor SIGKILLed mid-storm "
                  "(%d execs seen), no wedged tracees" % n_exec)
            return 0
        return fail("stranded storm survivors: %r" % procs_named("stormcat"))
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


if __name__ == "__main__":
    preflight()
    sys.exit({"a": test_a, "b": test_b, "c": test_c, "d": test_d}[WHICH]())

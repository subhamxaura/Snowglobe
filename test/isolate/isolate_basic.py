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
from sgtest_util import check_linux, grep_bytes, load_events, skip  # noqa: E402

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

        # (e) seccomp: ns-root HAS caps, so EPERM here proves the filter
        # (an unprivileged attempt would EPERM filter or not).
        out = os.path.join(work, "e.sgr")
        r = run_sg_iso(
            [sys.executable, "-c",
             "import ctypes;libc=ctypes.CDLL(None,use_errno=True);"
             "r=libc.mount(b'none',b'/tmp',b'tmpfs',0,None);"
             "import errno,sys;print('rc=%d errno=%d'%(r,ctypes.get_errno()));"
             "sys.exit(0 if (r==-1 and ctypes.get_errno()==errno.EPERM) else 1)"],
            out)
        if r.returncode != 0 or "rc=-1 errno=1" not in (r.stdout or ""):
            print("FAIL: seccomp mount not EPERM: rc=%d out=%s err=%s"
                  % (r.returncode, r.stdout, r.stderr[-500:]))
            ok = False
        evs = load_events(out)
        if any(e.get("ev") == "trace.decode_error" for e in evs):
            print("FAIL: decode_error in seccomp trace")
            ok = False

        # (f) io_uring_setup dies SIGSYS (KILL, fail loud not silent).
        out = os.path.join(work, "f.sgr")
        r = run_sg_iso(
            [sys.executable, "-c",
             "import ctypes;ctypes.CDLL(None,use_errno=True).syscall(425,8,0)"],
            out)
        if r.returncode != 159:
            print("FAIL: io_uring_setup must exit 159 (SIGSYS), got %d:\n%s"
                  % (r.returncode, r.stderr[-500:]))
            ok = False
        evs = load_events(out)
        if not any(e.get("ev") == "proc.exit" and e.get("signal") == 31
                   for e in evs):
            print("FAIL: no proc.exit signal 31 for io_uring_setup")
            ok = False

        # (g) env masking: secret names stripped, allow-env + *_BASE_URL pass.
        repo2 = os.path.join(work, "repo2")
        os.makedirs(repo2)
        out = os.path.join(work, "g.sgr")
        env = {"AWS_SECRET_ACCESS_KEY": "supersecret123",
               "MYAPP_BASE_URL": "http://keep-me",
               "NORMAL_VAR": "hi"}
        r = subprocess.run(
            [SNOWGLOBE, "run", "--out=" + out, "--isolate",
             "--project=" + repo2, "--",
             "sh", "-c", "echo ${AWS_SECRET_ACCESS_KEY-unset} > %s/k; "
                         "echo $MYAPP_BASE_URL > %s/b; echo $NORMAL_VAR > %s/n"
                         % (repo2, repo2, repo2)],
            capture_output=True, text=True,
            env={**os.environ, **env}, timeout=120)
        if r.returncode != 0:
            print("FAIL: env run exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        else:
            up = os.path.join(out, "overlay", "upper")
            with open(os.path.join(up, "k")) as f:
                got = f.read()
            if got != "unset\n":
                print("FAIL: secret survived masking: %r" % got)
                ok = False
            with open(os.path.join(up, "b")) as f:
                if f.read() != "http://keep-me\n":
                    print("FAIL: *_BASE_URL did not pass")
                    ok = False
            with open(os.path.join(up, "n")) as f:
                if f.read() != "hi\n":
                    print("FAIL: normal var lost")
                    ok = False
            if grep_bytes(out, "supersecret"):
                print("FAIL: secret value present in trace: %s"
                      % grep_bytes(out, "supersecret"))
                ok = False

        # (h) --fs-rw: extra dir overlays (host clean, upper copy present).
        extra = os.path.join(work, "extra")
        os.makedirs(extra)
        out = os.path.join(work, "h.sgr")
        r = run_sg_iso(["sh", "-c", "echo rw-data > %s/f.txt" % extra], out,
                       flags=["--project=" + repo, "--fs-rw=" + extra])
        if r.returncode != 0:
            print("FAIL: fs-rw run exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif os.path.exists(os.path.join(extra, "f.txt")):
            print("FAIL: fs-rw write leaked to host")
            ok = False
        else:
            with open(os.path.join(out, "overlay", "fs-rw-0", "f.txt")) as f:
                if f.read() != "rw-data\n":
                    print("FAIL: fs-rw upper content wrong")
                    ok = False

        # (h2) denial visibility: / is a writable tmpfs, so only landlock
        # can refuse this write. Landlock deliberately answers EACCES
        # (13), not EPERM, to blend with DAC denials — pin that shape.
        # The run itself stays exit 0.
        out = os.path.join(work, "h2.sgr")
        probe = "/denied-%d" % os.getpid()
        r = run_sg_iso(["sh", "-c", "echo x > %s; echo inner-rc=$?" % probe],
                       out, flags=["--project=" + repo])
        if r.returncode != 0:
            print("FAIL: denial-probe run exited %d" % r.returncode)
            ok = False
        else:
            evs = load_events(out)
            denials = [e for e in evs if e.get("ev") == "fs.open" and
                       e.get("path") == probe and e.get("write") is True]
            if len(denials) != 1 or denials[0].get("ok") is not False:
                print("FAIL: no ok:false denial event: %s" % denials)
                ok = False
            elif denials[0].get("errno") != 13:
                print("FAIL: denial errno want 13 (landlock EACCES), got %s"
                      % denials[0])
                ok = False

        # (i) cgroup best-effort: exit 0 either way, note names the path.
        out = os.path.join(work, "i.sgr")
        r = run_sg_iso(["/bin/true"], out,
                       flags=["--memory-max=64M", "--pids-max=64"])
        if r.returncode != 0:
            print("FAIL: cgroup run exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif "cgroup" not in r.stderr.lower():
            print("FAIL: no cgroup note (applied or denied) in:\n%s" % r.stderr)
            ok = False
        else:
            print("cgroup path: " +
                  [l for l in r.stderr.splitlines() if "cgroup" in l.lower()][0])

        # (j) doctor --isolate: rows present, exit matches required-green.
        r = subprocess.run([SNOWGLOBE, "doctor", "--isolate"],
                           capture_output=True, text=True, timeout=120)
        req_red = [l for l in r.stdout.splitlines()
                   if " req " in (" " + l + " ") and " RED " in (" " + l + " ")]
        for need in ("userns", "mount+overlay", "pidns", "seccomp", "landlock"):
            if need not in r.stdout:
                print("FAIL: doctor --isolate missing row " + need)
                ok = False
        want = 0 if not req_red else 69
        if r.returncode != want:
            print("FAIL: doctor --isolate exit %d, want %d (req RED: %s)"
                  % (r.returncode, want, req_red))
            ok = False

        print("PASS isolate_basic" if ok else "FAIL isolate_basic")
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

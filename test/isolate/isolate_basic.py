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

        # (k) seccomp Docker parity: 3 newly-blocked calls EPERM inside (mapped
        # root HAS caps, so EPERM proves the filter), success/different outside.
        # add_key: success outside (key serial) vs EPERM inside.
        # socket(AF_ALG): success outside vs EPERM inside (family filtering).
        # get_mempolicy: success outside vs EPERM inside (NUMA).
        # personality query still works both sides (allowed values).
        helper = os.path.join(repo, "sec-helper.py")
        with open(helper, "w") as f:
            f.write(
                "import ctypes, errno, socket\n"
                "libc=ctypes.CDLL(None,use_errno=True)\n"
                "libc.syscall.restype=ctypes.c_long\n"
                "import platform\n"
                "m=platform.machine()\n"
                "NR_ADD_KEY=248 if m=='x86_64' else 261\n"
                "NR_GETMEM=239\n"
                "ctypes.set_errno(0)\n"
                "r=libc.syscall(NR_ADD_KEY,b'user',b'sg-k',b'p',1,-4)\n"
                "print('add_key %d %d' % (r, ctypes.get_errno() if r==-1 else 0))\n"
                "try:\n"
                "    s=socket.socket(38,socket.SOCK_SEQPACKET,0)\n"
                "    print('alg 0 0')\n"
                "    s.close()\n"
                "except OSError as ex:\n"
                "    print('alg -1 %d' % ex.errno)\n"
                "import ctypes as C\n"
                "mode=C.c_int(0)\n"
                "ctypes.set_errno(0)\n"
                "r=libc.syscall(NR_GETMEM,C.byref(mode),0,0,0,0)\n"
                "print('getmem %d %d' % (r, ctypes.get_errno() if r==-1 else 0))\n"
                "ctypes.set_errno(0)\n"
                "r=libc.syscall(135 if m=='x86_64' else 92,0xFFFFFFFF)\n"
                "print('persq %d %d' % (r, ctypes.get_errno() if r==-1 else 0))\n"
            )
        # Outside (no isolate): add_key success (rc>=0), alg success, getmem success, persq success.
        r = subprocess.run([sys.executable, helper], capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            print("FAIL: sec helper outside exited %d" % r.returncode)
            ok = False
        else:
            lines = dict(l.split(" ", 1) for l in r.stdout.strip().splitlines() if " " in l)
            # add_key outside: rc>=0 (key serial), errno 0
            try:
                rc, e = map(int, lines.get("add_key", "-99 -99").split())
                if rc < 0:
                    print("FAIL: add_key outside should succeed, got %s" % lines.get("add_key"))
                    ok = False
            except Exception as ex:
                print("FAIL: add_key outside parse: %s (%s)" % (lines, ex))
                ok = False
            if lines.get("alg", "") != "0 0":
                print("FAIL: socket-ALG outside should succeed, got %s" % lines.get("alg"))
                ok = False
            try:
                rc, e = map(int, lines.get("getmem", "-99 -99").split())
                if rc != 0:
                    print("FAIL: get_mempolicy outside should succeed, got %s" % lines.get("getmem"))
                    ok = False
            except Exception as ex:
                print("FAIL: getmem outside parse: %s" % lines)
                ok = False
        # Inside: add_key EPERM (1), alg EPERM (1), getmem EPERM (1), persq success.
        out = os.path.join(work, "k.sgr")
        r = run_sg_iso([sys.executable, helper], out, flags=["--project=" + repo])
        if r.returncode != 0:
            print("FAIL: sec helper inside exited %d:\n%s" % (r.returncode, r.stderr[-500:]))
            ok = False
        else:
            lines = dict(l.split(" ", 1) for l in r.stdout.strip().splitlines() if " " in l)
            if lines.get("add_key", "") != "-1 1":
                print("FAIL: add_key inside want -1 1 (EPERM), got %s" % lines.get("add_key"))
                ok = False
            if lines.get("alg", "") != "-1 1":
                print("FAIL: socket-ALG inside want -1 1 (EPERM), got %s" % lines.get("alg"))
                ok = False
            if lines.get("getmem", "") != "-1 1":
                print("FAIL: get_mempolicy inside want -1 1 (EPERM), got %s" % lines.get("getmem"))
                ok = False
            try:
                rc, e = map(int, lines.get("persq", "-99 -99").split())
                if rc < 0:
                    print("FAIL: personality query must stay allowed, got %s" % lines.get("persq"))
                    ok = False
            except Exception:
                print("FAIL: persq inside parse: %s" % lines)
                ok = False

        # (l) prlimit fallback: child observes reduced ulimit -u (NPROC) and
        # ulimit -n (NOFILE); AS unlimited without --memory-max (Bun-safe).
        out = os.path.join(work, "l.sgr")
        r = run_sg_iso(["bash", "-c", "ulimit -u; ulimit -n; ulimit -v"], out,
                       flags=["--project=" + repo])
        if r.returncode != 0:
            print("FAIL: prlimit run exited %d" % r.returncode)
            ok = False
        else:
            vals = (r.stdout or "").strip().split()
            if len(vals) < 3:
                print("FAIL: prlimit output short: %r" % r.stdout)
                ok = False
            else:
                try:
                    nproc = int(vals[0])
                    if nproc > 5000:
                        print("FAIL: ulimit -u not reduced inside: %s" % vals[0])
                        ok = False
                except ValueError:
                    if vals[0] != "unlimited":
                        print("FAIL: ulimit -u parse: %r" % vals)
                        ok = False
                if vals[1] != "1024":
                    print("FAIL: ulimit -n want 1024, got %s" % vals[1])
                    ok = False
                if vals[2] != "unlimited":
                    print("FAIL: ulimit -v must stay unlimited without --memory-max (Bun), got %s"
                          % vals[2])
                    ok = False
            if "prlimit fallback active" not in r.stderr:
                print("FAIL: no prlimit fallback note in:\n%s" % r.stderr)
                ok = False
        # With --memory-max, AS limits apply (ulimit -v finite).
        # Absolute /bin/bash: no PATH search, so no allocator activity
        # between the agent's pre-exec RLIMIT_AS and execve (old-toolchain
        # ASan runtimes can die on a post-limit mmap).
        out = os.path.join(work, "l2.sgr")
        r = run_sg_iso(["/bin/bash", "-c", "ulimit -v"], out,
                       flags=["--project=" + repo, "--memory-max=64M"])
        if r.returncode != 0:
            print("FAIL: prlimit AS run exited %d" % r.returncode)
            ok = False
        elif (r.stdout or "").strip() == "unlimited":
            print("FAIL: ulimit -v must be finite with --memory-max=64M")
            ok = False

        # (m) secret-path masks: ~/.ssh/.aws/.gnupg empty inside, visible with
        # --allow-path; run.meta records masks.
        fakehome = os.path.join(work, "fakehome")
        os.makedirs(os.path.join(fakehome, ".ssh"), exist_ok=True)
        os.makedirs(os.path.join(fakehome, ".aws"), exist_ok=True)
        with open(os.path.join(fakehome, ".ssh", "secret.txt"), "w") as f:
            f.write("TOPSECRET")
        env = dict(os.environ)
        env["HOME"] = fakehome
        out = os.path.join(work, "m.sgr")
        r = subprocess.run(
            [SNOWGLOBE, "run", "--out=" + out, "--isolate", "--project=" + repo,
             "--", "sh", "-c", "ls $HOME/.ssh 2>&1; cat $HOME/.ssh/secret.txt 2>&1; true"],
            capture_output=True, text=True, env=env, timeout=120)
        if r.returncode != 0:
            print("FAIL: mask run exited %d" % r.returncode)
            ok = False
        elif "TOPSECRET" in (r.stdout or ""):
            print("FAIL: masked .ssh content visible:\n%s" % r.stdout)
            ok = False
        evs = load_events(out)
        metas = [e for e in evs if e.get("ev") == "run.meta"]
        if len(metas) != 1 or not isinstance(metas[0].get("masks"), list):
            print("FAIL: run.meta masks missing: %s" % metas)
            ok = False
        elif not any(".ssh" in m for m in metas[0].get("masks")):
            print("FAIL: run.meta masks lack .ssh: %s" % metas[0].get("masks"))
            ok = False
        # Exempted: visible.
        out = os.path.join(work, "m2.sgr")
        r = subprocess.run(
            [SNOWGLOBE, "run", "--out=" + out, "--isolate", "--project=" + repo,
             "--allow-path=" + os.path.join(fakehome, ".ssh"),
             "--", "sh", "-c", "cat $HOME/.ssh/secret.txt 2>&1"],
            capture_output=True, text=True, env=env, timeout=120)
        if r.returncode != 0 or "TOPSECRET" not in (r.stdout or ""):
            print("FAIL: --allow-path .ssh should be visible, rc=%d out=%s err=%s"
                  % (r.returncode, r.stdout, r.stderr[-500:]))
            ok = False

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
        # Block 3: procfs row text + cgroup prlimit text.
        if "REQUIRED for Bun/Node-class runtimes (issue #4)" not in r.stdout:
            print("FAIL: doctor procfs row lacks Bun/Node REQUIRED text:\n%s" % r.stdout)
            ok = False
        if "prlimit fallback active" not in r.stdout:
            print("FAIL: doctor lacks prlimit fallback active:\n%s" % r.stdout)
            ok = False

        print("PASS isolate_basic" if ok else "FAIL isolate_basic")
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

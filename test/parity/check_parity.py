#!/usr/bin/env python3
"""Phase 4 Block 2: ptrace-vs-notify parity for one golden scenario.

Usage: check_parity.py <snowglobe> <scenario-dir> <repo> <helpers-dir>

Runs the scenario under BOTH backends, normalizes both streams with
test/normalize.py, and verifies the ADR-0009 parity relation:

1. Subsequence: every ptrace event (mapped) appears in the notify stream
   in order. Notify may carry EXTRAS, and every extra must be an
   fs.open (failed read probes the ptrace backend filters; notify
   records attempts with result_known:false).
2. Mapping: proc.exec_failed{path} <-> proc.exec{path} (argv ignored:
   ptrace records no argv on failure); all other kinds compare full
   normalized dicts minus outcome keys (ok/errno/initiated/fd are
   ptrace-only; result_known is notify-only).
3. Shape (raw notify stream): every outcome-class event
   (fs.open/unlink/rmdir/rename/symlink/chmod, net.connect/sendto/bind/
   disconnect, proc.exec) carries result_known:false and no ok/errno/fd
   keys; every other kind carries no result_known. run.meta carries
   backend:notify. Zero trace.decode_error in either stream.

Exit 0 + PASS line, else FAIL with the first divergence (never silent).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)
NORM_PY = os.path.join(TEST_DIR, "normalize.py")

OUTCOME = {"fs.open", "fs.unlink", "fs.rmdir", "fs.rename", "fs.symlink",
           "fs.chmod", "net.connect", "net.sendto", "net.bind",
           "net.disconnect", "proc.exec"}
PTrace_ONLY = {"ok", "errno", "initiated", "fd"}


def sh(args, **kw):
    timeout = kw.pop("timeout", 120)
    return subprocess.run(args, capture_output=True, text=True,
                          timeout=timeout, **kw)


def backend_available(sg):
    """Preflight for notify: exit 69 (EX_UNAVAILABLE) means the kernel
    lacks user-notify — skip with a reason, never fail (AGENTS.md §5)."""
    work = tempfile.mkdtemp(prefix="sg-preflight-")
    try:
        r = sh([sg, "run", "--out=" + os.path.join(work, "p.sgr"),
                "--backend=notify", "--", "/bin/true"])
        return r.returncode != 69
    except OSError:
        return False
    finally:
        shutil.rmtree(work, ignore_errors=True)


def run_backend(sg, scn, repo, helpers, backend, work):
    out = os.path.join(work, backend + ".sgr")
    env = dict(os.environ, SG_HELPERS=helpers)
    # Same hermetic loader env as golden.py (see there): parity compares
    # live runs, so pollution would cancel out — but the runs must also
    # match the committed hermetic goldens, so scrub here too.
    for var in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "LD_DEBUG"):
        env.pop(var, None)
    r = sh([sg, "run", "--out=" + out, "--backend=" + backend, "--",
            os.path.join(scn, "run.sh")],
           cwd=scn, env=env)
    if r.returncode != 0:
        print("FAIL: scenario under %s exited %d\n%s%s"
              % (backend, r.returncode, r.stdout, r.stderr))
        return None
    return out


def normalize(run_dir, repo, helpers):
    cmd = [sys.executable, NORM_PY, os.path.join(run_dir, "events.jsonl"),
           "--repo", repo]
    if helpers:
        cmd += ["--helpers", helpers]
    n = sh(cmd, timeout=60)
    if n.returncode != 0:
        print("FAIL: normalize failed\n%s" % n.stderr)
        return None
    return [json.loads(l) for l in n.stdout.splitlines() if l.strip()]


def load_raw(run_dir):
    with open(os.path.join(run_dir, "events.jsonl")) as f:
        return [json.loads(l) for l in f if l.strip()]


def map_ptrace(e):
    """Comparable key for a normalized ptrace event."""
    ev = e.get("ev")
    if ev == "proc.exec_failed":
        return ("proc.exec", e.get("path"), None)
    if ev == "proc.exec":
        return ("proc.exec", e.get("path"), tuple(e.get("argv", [])))
    if ev == "fs.open":
        return ("fs.open", e.get("path"), e.get("write"), e.get("create"),
                e.get("trunc"), e.get("tmpfile", False))
    if ev in ("fs.unlink", "fs.rmdir"):
        return (ev, e.get("path"))
    if ev == "fs.rename":
        return (ev, e.get("from"), e.get("to"))
    if ev == "fs.mkdir":
        return (ev, e.get("path"))
    if ev == "fs.symlink":
        return (ev, e.get("target"), e.get("path"))
    if ev == "fs.chmod":
        return (ev, e.get("path"), e.get("mode"))
    if ev in ("net.connect", "net.sendto", "net.bind"):
        return (ev, e.get("family"), e.get("addr"), e.get("ip"),
                e.get("port"), e.get("path"))
    if ev == "net.disconnect":
        return (ev,)
    if ev == "proc.start":
        return (ev, e.get("pid"), e.get("ppid"), e.get("thread", False))
    if ev == "proc.exit":
        if e.get("vanished") is True:
            return (ev, "vanished")
        return (ev, e.get("code"), e.get("signal"))
    if ev == "run.meta":
        return (ev,)
    if ev in ("llm.request", "llm.response", "llm.chunk"):
        return (ev, e.get("id"), e.get("status", e.get("model")))
    return (ev, json.dumps(e, sort_keys=True))


def map_notify(e):
    ev = e.get("ev")
    if ev == "proc.exec":
        return ("proc.exec", e.get("path"), tuple(e.get("argv", [])))
    return map_ptrace(e)


def main():
    sg, scn, repo = sys.argv[1], sys.argv[2], sys.argv[3]
    helpers = sys.argv[4] if len(sys.argv) > 4 else ""
    scn = os.path.abspath(scn)
    repo = os.path.abspath(repo)
    name = os.path.basename(scn.rstrip("/"))
    if not backend_available(sg):
        print("SKIP %s: kernel lacks seccomp user-notify" % name)
        return 0
    work = tempfile.mkdtemp(prefix="sg-parity-")
    ok = True
    try:
        pdir = run_backend(sg, scn, repo, helpers, "ptrace", work)
        ndir = run_backend(sg, scn, repo, helpers, "notify", work)
        if pdir is None or ndir is None:
            return 1
        praw = load_raw(pdir)
        nraw = load_raw(ndir)
        pnorm = normalize(pdir, repo, helpers)
        nnorm = normalize(ndir, repo, helpers)
        if pnorm is None or nnorm is None:
            return 1

        # 0. No silent drops anywhere: decode errors fail both streams.
        for tag, raw in (("ptrace", praw), ("notify", nraw)):
            bad = [e for e in raw if e.get("ev") == "trace.decode_error"]
            if bad:
                print("FAIL %s: %d trace.decode_error (first: %s)"
                      % (name, len(bad), json.dumps(bad[0], sort_keys=True)))
                ok = False

        # 1. Raw notify shape: result_known:false exactly on outcome
        #    events; no ok/errno/fd keys outside decode_error.
        for e in nraw:
            ev = e.get("ev")
            if ev == "trace.decode_error":
                continue
            has_known = e.get("result_known") is False
            if ev in OUTCOME:
                if not has_known:
                    print("FAIL %s: notify %s lacks result_known:false: %s"
                          % (name, ev, json.dumps(e, sort_keys=True)))
                    ok = False
            elif "result_known" in e:
                print("FAIL %s: unexpected result_known on %s: %s"
                      % (name, ev, json.dumps(e, sort_keys=True)))
                ok = False
            for k in PTrace_ONLY:
                if k in e:
                    print("FAIL %s: notify %s carries ptrace-only key %r: %s"
                          % (name, ev, k, json.dumps(e, sort_keys=True)))
                    ok = False
        metas = [e for e in nraw if e.get("ev") == "run.meta"]
        if len(metas) != 1 or metas[0].get("backend") != "notify":
            print("FAIL %s: run.meta backend != notify: %s" % (name, metas))
            ok = False
        pmetas = [e for e in praw if e.get("ev") == "run.meta"]
        if len(pmetas) != 1 or pmetas[0].get("backend") != "ptrace":
            print("FAIL %s: ptrace run.meta backend != ptrace" % name)
            ok = False

        # 2. Subsequence: mapped ptrace stream inside mapped notify stream.
        pkeys = [map_ptrace(e) for e in pnorm]
        nkeys = [map_notify(e) for e in nnorm]
        # exec_failed maps without argv: match notify entries by path.
        j = 0
        matched = 0
        for i, pk in enumerate(pkeys):
            found = None
            while j < len(nkeys):
                nk = nkeys[j]
                j += 1
                if pk[0] == "proc.exec" and pk[2] is None:
                    if nk[0] == "proc.exec" and nk[1] == pk[1]:
                        found = nk
                        break
                elif nk == pk:
                    found = nk
                    break
            if found is None:
                print("FAIL %s: ptrace event %d has no notify counterpart: %s"
                      % (name, i, json.dumps(pnorm[i], sort_keys=True)))
                print("  remaining notify tail: %d events" % (len(nkeys) - j + 1))
                ok = False
                break
            matched += 1

        # 3. Notify extras must all be fs.open (unfiltered failed probes).
        if ok:
            # Re-walk to collect unmatched notify indexes.
            used = set()
            j = 0
            for pk in pkeys:
                while j < len(nkeys):
                    nk = nkeys[j]
                    j += 1
                    if pk[0] == "proc.exec" and pk[2] is None:
                        if nk[0] == "proc.exec" and nk[1] == pk[1]:
                            used.add(j - 1)
                            break
                    elif nk == pk:
                        used.add(j - 1)
                        break
            for k, (e, nk) in enumerate(zip(nnorm, nkeys)):
                if k not in used and nk[0] != "fs.open":
                    print("FAIL %s: non-open notify extra: %s"
                          % (name, json.dumps(e, sort_keys=True)))
                    ok = False
            n_extra = sum(1 for k in range(len(nkeys)) if k not in used)
            print("PASS %s: %d ptrace events all matched in %d notify events "
                  "(%d open extras)" % (name, len(pkeys), len(nkeys), n_extra))
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Phase 3: diff/apply/compare goldens over scripted --isolate runs.

Covers: baseline write + excludes, diff --stat/--patch goldens (A/M/D/R,
unicode/symlink/empty/large/chmod-skip/.git-tamper), apply dry-run golden,
apply-clean (files land, rerun diff empty), apply-conflict 65 with
byte-identical zero-writes proof, symlink-escape rejection, exit codes
(64/65/69), compare table/json goldens, non-isolate honest-mode golden.
Regenerate goldens with SNOWGLOBE_UPDATE_GOLDENS=1 (only when intended).
Usage: diff_apply_golden.py <snowglobe-bin>. Stdlib only.
"""
import difflib
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "proxy"))
from sgtest_util import check_linux, skip  # noqa: E402

SNOWGLOBE = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = os.path.join(HERE, "expected")
UPDATE = os.environ.get("SNOWGLOBE_UPDATE_GOLDENS") == "1"


def sh(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, timeout=kw.get("timeout", 120),
                          **{k: v for k, v in kw.items() if k != "timeout"})


def run_iso(proj, out, script, extra=()):
    return sh([SNOWGLOBE, "run", "--out=" + out, "--isolate", "--project=" + proj,
               "--no-llm-proxy", *extra, "--", "sh", "-c", script])


def tree_hashes(root):
    h = {}
    for dp, dn, fn in os.walk(root, followlinks=False):
        # Never descend through symlinked dirs (test-harness safety).
        dn[:] = [d for d in dn if not os.path.islink(os.path.join(dp, d))]
        for x in fn:
            p = os.path.join(dp, x)
            if os.path.islink(p):
                with open(p, "rb") as f:
                    h[os.path.relpath(p, root)] = ("link:", os.readlink(p))
            else:
                with open(p, "rb") as f:
                    h[os.path.relpath(p, root)] = ("file:", hashlib.sha256(f.read()).hexdigest())
    return h


def norm(work, s):
    return s.replace(work, "$WORK")


def check_golden(name, actual):
    path = os.path.join(EXPECTED, name)
    if UPDATE:
        with open(path, "w") as f:
            f.write(actual)
        print("UPDATED %s (%d bytes; only if intended)" % (name, len(actual)))
        return True
    with open(path) as f:
        expected = f.read()
    if expected != actual:
        print("FAIL golden %s mismatch:" % name)
        for line in difflib.unified_diff(expected.splitlines(), actual.splitlines(),
                                         "expected", "actual", lineterm=""):
            print(line)
        return False
    return True


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="sg-diff-")
    ok = True
    try:
        # Capability probe (mirrors isolate_basic.try_isolate).
        probe = os.path.join(work, "probe.sgr")
        r = sh([SNOWGLOBE, "run", "--out=" + probe, "--isolate", "--", "/bin/true"])
        if r.returncode == 69:
            skip("no --isolate capability here: " + r.stderr.strip()[-200:])
        if r.returncode != 0:
            print("FAIL: isolate smoke exited %d:\n%s" % (r.returncode, r.stderr))
            return 1

        # ---- run1: rich op mix incl. .git tamper (diff goldens + apply reject).
        proj1 = os.path.join(work, "proj1")
        os.makedirs(os.path.join(proj1, "sub"))
        open(os.path.join(proj1, "keep.txt"), "w").write("keep\n")
        open(os.path.join(proj1, "mod.txt"), "w").write("v1\nline2\nline3\n")
        open(os.path.join(proj1, "del.txt"), "w").write("bye\n")
        open(os.path.join(proj1, "old.txt"), "w").write("same-bytes\n")
        open(os.path.join(proj1, "empty.txt"), "w").write("")
        open(os.path.join(proj1, "uni-\u00e9.txt"), "w").write("u1\n")
        open(os.path.join(proj1, "chmodme.txt"), "w").write("same\n")
        with open(os.path.join(proj1, "big.txt"), "w") as f:
            for i in range(3000):
                f.write("line %d\n" % i)
        os.symlink("keep.txt", os.path.join(proj1, "link.txt"))
        open(os.path.join(proj1, "sub", "nested.txt"), "w").write("n\n")
        os.makedirs(os.path.join(proj1, ".git"))
        open(os.path.join(proj1, ".git", "config"), "w").write("[core]\n")
        os.makedirs(os.path.join(proj1, "node_modules"))
        open(os.path.join(proj1, "node_modules", "dep.js"), "w").write("y\n")
        r = run_iso(
            proj1, os.path.join(work, "r1.sgr"),
            "echo new > %(p)s/new.txt; "
            "printf 'v1\\nline2 MOD\\nline3\\n' > %(p)s/mod.txt; "
            "rm %(p)s/del.txt; mv %(p)s/old.txt %(p)s/renamed.txt; "
            "ln -sfn mod.txt %(p)s/link.txt; "
            "echo u2 >> %(p)s/uni-\u00e9.txt; "
            "ln -s keep.txt %(p)s/newlink.txt; "
            "for i in 3000 3001 3002 3003 3004; do echo append $i >> %(p)s/big.txt; done; "
            "chmod 600 %(p)s/chmodme.txt; "
            "echo tampered > %(p)s/.git/config; "
            "echo dep2 > %(p)s/node_modules/dep.js" % {"p": proj1})
        if r.returncode != 0:
            print("FAIL: run1 exited %d:\n%s" % (r.returncode, r.stderr))
            return 1
        r1 = os.path.join(work, "r1.sgr")

        # Baseline excludes verified (default + custom via second run below).
        with open(os.path.join(r1, "baseline.json")) as f:
            bl = json.load(f)
        for bad in (".git/config", "node_modules/dep.js"):
            if bad in bl["files"]:
                print("FAIL: baseline contains excluded %s" % bad)
                ok = False
        if "mod.txt" not in bl["files"]:
            print("FAIL: baseline missing mod.txt")
            ok = False

        # diff --stat golden.
        r = sh([SNOWGLOBE, "diff", r1])
        if r.returncode != 0:
            print("FAIL: diff r1 exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif not check_golden("stat1.txt", norm(work, r.stderr)):
            ok = False
        if "chmodme.txt" in r.stderr:
            print("FAIL: chmod-only file reported as a change")
            ok = False
        # Excluded subtrees stay invisible (replayed in the upper walk)...
        if "node_modules" in r.stderr or ".snowglobe" in r.stderr:
            print("FAIL: excluded path leaked into diff")
            ok = False
        # ...except .git, which is always reviewed (apply refuses it).
        if ".git/config" not in r.stderr:
            print("FAIL: .git tamper hidden from review:\n%s" % r.stderr)
            ok = False

        # diff --patch golden.
        patch1 = os.path.join(work, "p1.patch")
        r = sh([SNOWGLOBE, "diff", r1, "--patch=" + patch1])
        if r.returncode != 0:
            print("FAIL: diff --patch exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        else:
            with open(patch1) as f:
                if not check_golden("patch1.txt", norm(work, f.read())):
                    ok = False
            # Persisted review artifacts (AGENTS.md §3): every content diff
            # writes fs/diff.patch + fs/summary.json into the run dir.
            # The persisted patch must equal the user --patch file.
            persisted = os.path.join(r1, "fs", "diff.patch")
            summary = os.path.join(r1, "fs", "summary.json")
            if not os.path.isfile(persisted):
                print("FAIL: diff did not persist fs/diff.patch")
                ok = False
            elif open(persisted, "rb").read() != open(patch1, "rb").read():
                print("FAIL: fs/diff.patch differs from --patch file")
                ok = False
            if not os.path.isfile(summary):
                print("FAIL: diff did not persist fs/summary.json")
                ok = False
            else:
                try:
                    sj = json.load(open(summary))
                    n = sj.get("counts", {})
                    total = sum(n.values()) if isinstance(n, dict) else -1
                    if sj.get("version") != 1 or total != len(sj.get("changes", [])):
                        print("FAIL: fs/summary.json counts mismatch: %s" % sj)
                        ok = False
                except Exception as e:
                    print("FAIL: fs/summary.json unreadable: %s" % e)
                    ok = False

        # apply --dry-run golden (rel paths only: no normalization needed).
        r = sh([SNOWGLOBE, "apply", r1, "--dry-run"])
        if r.returncode != 65:
            print("FAIL: apply dry-run on .git-tampered run want 65, got %d:\n%s"
                  % (r.returncode, r.stderr))
            ok = False
        elif not check_golden("apply_dry1.txt", r.stderr):
            ok = False

        # Real apply aborts 65 with byte-identical zero writes.
        before = tree_hashes(proj1)
        r = sh([SNOWGLOBE, "apply", r1, "--yes"])
        if r.returncode != 65:
            print("FAIL: apply on .git-tampered run want 65, got %d:\n%s"
                  % (r.returncode, r.stderr))
            ok = False
        elif tree_hashes(proj1) != before:
            print("FAIL: apply wrote despite abort")
            ok = False
        if ".git" not in r.stderr or "REJECTED" not in r.stderr:
            print("FAIL: abort report lacks .git REJECTED:\n%s" % r.stderr)
            ok = False

        # Symlink-escape rejection: a run whose change sits under sub/,
        # then the host swaps sub/ for a symlink at apply time. The write
        # must be refused (65) with the victim tree untouched.
        projS = os.path.join(work, "projS")
        os.makedirs(os.path.join(projS, "sub"))
        open(os.path.join(projS, "sub", "real.txt"), "w").write("real\n")
        r = run_iso(projS, os.path.join(work, "rS.sgr"),
                    "echo evil > %(p)s/sub/evil.txt" % {"p": projS})
        if r.returncode != 0:
            print("FAIL: runS exited %d" % r.returncode)
            ok = False
        else:
            victim = os.path.join(work, "victim")
            os.makedirs(victim)
            open(os.path.join(victim, "keep.txt"), "w").write("keep\n")
            shutil.rmtree(os.path.join(projS, "sub"))
            os.symlink(victim, os.path.join(projS, "sub"))
            beforeS = tree_hashes(projS)
            beforeV = tree_hashes(victim)
            r = sh([SNOWGLOBE, "apply", os.path.join(work, "rS.sgr"), "--yes"])
            if r.returncode != 65 or "symlink" not in r.stderr.lower():
                print("FAIL: symlink-escape want 65 + symlink note, got %d:\n%s"
                      % (r.returncode, r.stderr))
                ok = False
            elif tree_hashes(victim) != beforeV:
                print("FAIL: symlink-escape wrote through to the victim")
                ok = False
            # Nothing at all may change (the swap predates the snapshot).
            if tree_hashes(projS) != beforeS:
                print("FAIL: symlink-escape changed the project tree")
                ok = False

        # Exit codes: no-such-run 64; empty dir (no baseline/events) 69.
        r = sh([SNOWGLOBE, "diff", os.path.join(work, "nope.sgr")])
        if r.returncode != 64:
            print("FAIL: diff missing run want 64, got %d" % r.returncode)
            ok = False
        os.makedirs(os.path.join(work, "empty.sgr"))
        r = sh([SNOWGLOBE, "diff", os.path.join(work, "empty.sgr")])
        if r.returncode != 69:
            print("FAIL: diff no-artifact run want 69, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        r = sh([SNOWGLOBE, "apply", os.path.join(work, "empty.sgr"), "--yes"])
        if r.returncode != 69:
            print("FAIL: apply no-artifact run want 69, got %d" % r.returncode)
            ok = False

        # ---- run2: clean ops (apply lands, rerun empty, custom excludes).
        proj2 = os.path.join(work, "proj2")
        os.makedirs(proj2)
        open(os.path.join(proj2, "a.txt"), "w").write("a1\na2\n")
        open(os.path.join(proj2, "b.txt"), "w").write("gone\n")
        os.makedirs(os.path.join(proj2, "vendor"))
        open(os.path.join(proj2, "vendor", "v.js"), "w").write("v\n")
        os.symlink("a.txt", os.path.join(proj2, "l.txt"))
        r = run_iso(
            proj2, os.path.join(work, "r2.sgr"),
            "echo hello > %(p)s/new2.txt; echo CHANGED > %(p)s/a.txt; rm %(p)s/b.txt; "
            "echo x > %(p)s/vendor/v.js" % {"p": proj2},
            extra=("--baseline-exclude=vendor",))
        if r.returncode != 0:
            print("FAIL: run2 exited %d:\n%s" % (r.returncode, r.stderr))
            return 1
        r2 = os.path.join(work, "r2.sgr")
        with open(os.path.join(r2, "baseline.json")) as f:
            bl2 = json.load(f)
        if "vendor/v.js" in bl2["files"]:
            print("FAIL: custom --baseline-exclude not honored")
            ok = False
        r = sh([SNOWGLOBE, "diff", r2])
        if "vendor" in r.stderr:
            print("FAIL: excluded vendor/ in diff:\n%s" % r.stderr)
            ok = False
        r = sh([SNOWGLOBE, "apply", r2, "--dry-run"])
        if r.returncode != 0:
            print("FAIL: dry-run clean want 0, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        r = sh([SNOWGLOBE, "apply", r2, "--yes"])
        if r.returncode != 0:
            print("FAIL: apply clean want 0, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        h = tree_hashes(proj2)
        if h.get("new2.txt", (None, None))[1] != hashlib.sha256(b"hello\n").hexdigest():
            print("FAIL: applied new2.txt content wrong")
            ok = False
        if h.get("a.txt", (None, None))[1] != hashlib.sha256(b"CHANGED\n").hexdigest():
            print("FAIL: applied a.txt content wrong")
            ok = False
        if "b.txt" in h:
            print("FAIL: whiteout delete not applied")
            ok = False
        r = sh([SNOWGLOBE, "diff", r2])
        if r.returncode != 0 or "no pending changes" not in r.stderr:
            print("FAIL: rerun diff want empty, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False

        # Conflict: run3 baselines post-apply state; host dirties the file.
        r = run_iso(proj2, os.path.join(work, "r3.sgr"),
                    "echo agent > %(p)s/a.txt" % {"p": proj2})
        if r.returncode != 0:
            print("FAIL: run3 exited %d" % r.returncode)
            return 1
        r3 = os.path.join(work, "r3.sgr")
        open(os.path.join(proj2, "a.txt"), "w").write("host-dirty\n")
        before = tree_hashes(proj2)
        r = sh([SNOWGLOBE, "apply", r3, "--yes"])
        if r.returncode != 65:
            print("FAIL: conflict apply want 65, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif tree_hashes(proj2) != before:
            print("FAIL: conflict apply wrote")
            ok = False
        elif "CONFLICT" not in r.stderr:
            print("FAIL: conflict report lacks CONFLICT:\n%s" % r.stderr)
            ok = False

        # Dir whiteouts, crafted deterministically: agent `rm -rf` of an
        # emptied merged dir fails EIO on this kernel (see limitations),
        # so whiteouts are hand-made with mapped-root mknod (char 0/0),
        # which the dir-delete path consumes exactly like agent deletes.
        # Flow: dry-run clean, then host-added extra forces conflict 65
        # with zero writes, then clean apply lands and rerun is empty.
        proj6 = os.path.join(work, "proj6")
        os.makedirs(os.path.join(proj6, "sub"))
        open(os.path.join(proj6, "sub", "f.txt"), "w").write("f\n")
        open(os.path.join(proj6, "gone.txt"), "w").write("g\n")
        r = run_iso(proj6, os.path.join(work, "r6.sgr"), "/bin/true")
        if r.returncode != 0:
            print("FAIL: run6 exited %d" % r.returncode)
            return 1
        r6 = os.path.join(work, "r6.sgr")
        upper6 = os.path.join(r6, "overlay", "upper")
        r = sh(["unshare", "--map-user=0", "--map-group=0", "-Urm", "python3", "-c",
                "import os; os.mknod(%r, 0o020000, 0); os.mknod(%r, 0o020000, 0); "
                "open(%r, 'w').write('added\\n')" %
                (os.path.join(upper6, "sub"), os.path.join(upper6, "gone.txt"),
                 os.path.join(upper6, "added.txt"))])
        if r.returncode != 0:
            print("FAIL: whiteout crafting:\n%s" % r.stderr)
            return 1
        r = sh([SNOWGLOBE, "diff", r6])
        if r.returncode != 0:
            print("FAIL: diff crafted run exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        else:
            for want in ("D project gone.txt", "D project sub", "A project added.txt"):
                if want not in r.stderr:
                    print("FAIL: crafted diff lacks %r:\n%s" % (want, r.stderr))
                    ok = False
        r = sh([SNOWGLOBE, "apply", r6, "--dry-run"])
        if r.returncode != 0:
            print("FAIL: crafted dry-run want 0, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        # Host extras under the whiteouted dir: conflict 65, zero writes
        # (extras would otherwise vanish silently in the rmtree).
        open(os.path.join(proj6, "sub", "extra.txt"), "w").write("host-added\n")
        before6 = tree_hashes(proj6)
        r = sh([SNOWGLOBE, "apply", r6, "--yes"])
        if r.returncode != 65:
            print("FAIL: extras conflict want 65, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif tree_hashes(proj6) != before6:
            print("FAIL: extras conflict wrote")
            ok = False
        elif "CONFLICT sub:" not in r.stderr:
            print("FAIL: extras report lacks the sub conflict:\n%s" % r.stderr)
            ok = False
        os.unlink(os.path.join(proj6, "sub", "extra.txt"))
        r = sh([SNOWGLOBE, "apply", r6, "--yes"])
        if r.returncode != 0:
            print("FAIL: crafted apply want 0, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif os.path.exists(os.path.join(proj6, "sub")) or \
                os.path.exists(os.path.join(proj6, "gone.txt")):
            print("FAIL: crafted apply did not land deletes")
            ok = False
        elif open(os.path.join(proj6, "added.txt")).read() != "added\n":
            print("FAIL: crafted apply did not land the add")
            ok = False
        r = sh([SNOWGLOBE, "diff", r6])
        if r.returncode != 0 or "no pending changes" not in r.stderr:
            print("FAIL: crafted rerun want empty, got %d:\n%s" % (r.returncode, r.stderr))
            ok = False

        # ---- compare: run2 (A a.txt?/new2/b.txt) vs run4 (different ops).
        proj3 = os.path.join(work, "proj3")
        os.makedirs(proj3)
        open(os.path.join(proj3, "a.txt"), "w").write("same-base\n")
        open(os.path.join(proj3, "c.txt"), "w").write("c-base\n")
        r = run_iso(proj3, os.path.join(work, "r4.sgr"),
                    "echo other > %(p)s/other.txt; echo SAME > %(p)s/c.txt; "
                    "echo hello > %(p)s/new2.txt; echo DIFF >> %(p)s/a.txt" % {"p": proj3})
        if r.returncode != 0:
            print("FAIL: run4 exited %d" % r.returncode)
            return 1
        r4 = os.path.join(work, "r4.sgr")
        r = sh([SNOWGLOBE, "compare", r2, r4])
        if r.returncode != 0:
            print("FAIL: compare exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif not check_golden("compare.txt", r.stderr):
            ok = False
        r = sh([SNOWGLOBE, "compare", r2, r4, "--json"])
        if r.returncode != 0:
            print("FAIL: compare --json exited %d" % r.returncode)
            ok = False
        else:
            try:
                rows = json.loads(r.stdout)
                assert isinstance(rows, list) and all(
                    set(x) == {"scope", "path", "inA", "inB", "same", "note"} for x in rows)
            except Exception as ex:
                print("FAIL: compare --json shape: %s\n%s" % (ex, r.stdout))
                ok = False
            else:
                if not check_golden("compare.json", r.stdout):
                    ok = False
        r = sh([SNOWGLOBE, "compare", r2, r4, "--stat"])
        if r.returncode != 0 or "path(s)" not in r.stderr:
            print("FAIL: compare --stat:\n%s" % r.stderr)
            ok = False
        r = sh([SNOWGLOBE, "compare", r2, os.path.join(work, "empty.sgr")])
        if r.returncode != 69:
            print("FAIL: compare bad run want 69, got %d" % r.returncode)
            ok = False

        # ---- non-isolate honest mode golden (absolute paths normalized).
        proj5 = os.path.join(work, "proj5")
        os.makedirs(proj5)
        open(os.path.join(proj5, "w.txt"), "w").write("w0\n")
        open(os.path.join(proj5, "gone.txt"), "w").write("g\n")
        r = sh([SNOWGLOBE, "run", "--out=" + os.path.join(work, "r5.sgr"),
                "--no-llm-proxy", "--project=" + proj5, "--",
                "sh", "-c",
                "echo n > %(p)s/created.txt; echo m >> %(p)s/w.txt; "
                "rm %(p)s/gone.txt; mv %(p)s/w.txt %(p)s/renamed.txt; "
                "mkdir %(p)s/newdir" % {"p": proj5}])
        if r.returncode != 0:
            print("FAIL: run5 exited %d" % r.returncode)
            return 1
        r = sh([SNOWGLOBE, "diff", os.path.join(work, "r5.sgr")])
        if r.returncode != 0:
            print("FAIL: event-mode diff exited %d:\n%s" % (r.returncode, r.stderr))
            ok = False
        elif not check_golden("eventmode.txt", norm(work, r.stderr)):
            ok = False

        print("PASS diff_apply_golden" if ok else "FAIL diff_apply_golden")
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

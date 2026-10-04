#!/usr/bin/env python3
"""Linker determinism: `snowglobe link` twice over fixture copies is
byte-identical; --check is 0 when current, 3 when missing. Stdlib only.
Usage: link_determinism.py <snowglobe-bin> <fixtures-real-dir>.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

SNOWGLOBE, REALDIR = sys.argv[1], sys.argv[2]

# fixture → (expected turns, expected probes excluded)
CASES = {
    "toy-agent-3turn": (3, 0),
    "claude-code-1-error": (11, 1),
}


def run(*args):
    p = subprocess.run([SNOWGLOBE, *args], capture_output=True, text=True,
                       timeout=120)
    return p.returncode, p.stderr


def main():
    for name, (want_turns, want_probes) in CASES.items():
        with tempfile.TemporaryDirectory(prefix="sglink-") as tmp:
            run_dir = os.path.join(tmp, name)
            shutil.copytree(os.path.join(REALDIR, name), run_dir)
            # --check before link: missing file is STALE (exit 3)
            rc, err = run("link", "--check", run_dir)
            assert rc == 3, (name, rc, err)
            assert "STALE" in err, (name, err)
            # link twice → byte-identical links.json
            rc, err = run("link", run_dir)
            assert rc == 0, (name, rc, err)
            with open(os.path.join(run_dir, "links.json"), "rb") as f:
                first = f.read()
            rc, err = run("link", run_dir)
            assert rc == 0, (name, rc, err)
            with open(os.path.join(run_dir, "links.json"), "rb") as f:
                assert f.read() == first, name
            # shape: version 1, expected turn/probe counts
            doc = json.loads(first.decode("utf-8"))
            assert doc["version"] == 1, name
            assert len(doc["turns"]) == want_turns, (name, len(doc["turns"]))
            assert 'probes excluded: %d' % want_probes in err, (name, err)
            # --check after link: CURRENT (exit 0)
            rc, err = run("link", "--check", run_dir)
            assert rc == 0, (name, rc, err)
            assert "CURRENT" in err, (name, err)
            print("link determinism OK: %s (%d turns)" % (name, want_turns))
    # usage errors stay EX_USAGE
    rc, err = run("link")
    assert rc == 64, (rc, err)
    rc, err = run("link", "/nonexistent-run-dir-xyz")
    assert rc == 64, (rc, err)
    print("link usage errors OK")


if __name__ == "__main__":
    sys.exit(main())

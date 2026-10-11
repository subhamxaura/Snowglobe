#!/usr/bin/env python3
"""share_bundle: happy path end to end.

Records a 2-turn toy run, shares it (default out), pins
byte-determinism (share twice -> identical sha256), verifies the run
and the bundle, extracts elsewhere and opens the extracted tree in
`view` (manifest + summary agree), then pins overlay exclusion (default)
and inclusion (--with-overlay) with hand-made upper content (no isolate
needed: the upper is just a directory until share packs it).
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from share_util import (check_linux, free_port, Mock, run_sg, run_cli,
                        two_turn_scenario, sha_file, load_events, View)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="tmp.")
    mock = None
    view = None
    try:
        port = free_port()
        wd = os.path.join(work, "w")
        os.makedirs(wd)
        durl = "http://127.0.0.1:%d/test-data" % port
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port,
                    two_turn_scenario(wd))
        orig = os.path.join(work, "orig.sgr")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, AGENT, "--workdir", wd,
                    "--data-url", durl],
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-share-001",
                                    "PYTHONDONTWRITEBYTECODE": "1"})
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        nev = len(load_events(orig))
        mock.cleanup()
        mock = None
        # Share (default out: orig.sgrx alongside the run).
        b1 = orig + "x"
        r = run_cli([SNOWGLOBE, "share", orig])
        if r.returncode != 0:
            print("FAIL: share exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        if "shared: %s" % b1 not in r.stderr or not os.path.exists(b1):
            print("FAIL: no shared epilogue/bundle:\n" + r.stderr)
            return 1
        # Byte-determinism: same run twice -> identical sha256.
        b2 = os.path.join(work, "second.sgrx")
        r = run_cli([SNOWGLOBE, "share", orig, "--out=" + b2])
        if r.returncode != 0:
            print("FAIL: re-share exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        if sha_file(b1) != sha_file(b2):
            print("FAIL: bundle not byte-deterministic")
            return 1
        # Verify the run and the bundle.
        r = run_cli([SNOWGLOBE, "verify", orig])
        if r.returncode != 0 or "OK (%d events)" % nev not in r.stderr:
            print("FAIL: verify run:\n%s%s" % (r.stdout, r.stderr))
            return 1
        r = run_cli([SNOWGLOBE, "verify", b1])
        if r.returncode != 0 or "OK (%d events)" % nev not in r.stderr:
            print("FAIL: verify bundle:\n%s%s" % (r.stdout, r.stderr))
            return 1
        # Extract elsewhere and open the extracted tree in view.
        xdir = os.path.join(work, "extracted")
        r = run_cli([SNOWGLOBE, "verify", b1, "--extract-to=" + xdir])
        if r.returncode != 0 or "extracted: %s" % xdir not in r.stderr:
            print("FAIL: extract:\n%s%s" % (r.stdout, r.stderr))
            return 1
        view = View(SNOWGLOBE, xdir)
        try:
            s, body = view.get("/api/manifest")
            man = json.loads(body)
            if s != 200 or man["event_count"] != nev:
                print("FAIL: extracted manifest: %s" % body[:200])
                return 1
            s, body = view.get("/api/summary")
            sm = json.loads(body)
            if s != 200 or sm["turns"] != 2:
                print("FAIL: extracted summary: %s" % body[:200])
                return 1
        finally:
            view.stop()
            view = None
        # Overlay: hand-made upper (no isolate needed for the pack path).
        upper = os.path.join(orig, "overlay", "upper")
        os.makedirs(os.path.join(upper, "sub"))
        with open(os.path.join(upper, "keep.txt"), "w") as f:
            f.write("keep")
        with open(os.path.join(upper, "sub", "more.txt"), "w") as f:
            f.write("more")
        b3 = os.path.join(work, "no-overlay.sgrx")
        r = run_cli([SNOWGLOBE, "share", orig, "--out=" + b3])
        if r.returncode != 0:
            print("FAIL: share w/o overlay flag:\n%s%s" % (r.stdout, r.stderr))
            return 1
        x3 = os.path.join(work, "x3")
        r = run_cli([SNOWGLOBE, "verify", b3, "--extract-to=" + x3])
        if r.returncode != 0:
            print("FAIL: extract no-overlay:\n%s%s" % (r.stdout, r.stderr))
            return 1
        if os.path.exists(os.path.join(x3, "overlay")):
            print("FAIL: overlay shipped without --with-overlay")
            return 1
        b4 = os.path.join(work, "with-overlay.sgrx")
        r = run_cli([SNOWGLOBE, "share", orig, "--out=" + b4,
                     "--with-overlay"])
        if r.returncode != 0:
            print("FAIL: share --with-overlay:\n%s%s" % (r.stdout, r.stderr))
            return 1
        x4 = os.path.join(work, "x4")
        r = run_cli([SNOWGLOBE, "verify", b4, "--extract-to=" + x4])
        if r.returncode != 0:
            print("FAIL: extract with-overlay:\n%s%s" % (r.stdout, r.stderr))
            return 1
        with open(os.path.join(x4, "overlay", "upper", "keep.txt")) as f:
            if f.read() != "keep":
                print("FAIL: overlay content wrong")
                return 1
        with open(os.path.join(x4, "overlay", "upper", "sub", "more.txt")) as f:
            if f.read() != "more":
                print("FAIL: overlay nested content wrong")
                return 1
        print("PASS share_bundle: pack, determinism, verify, view, overlay")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        if view is not None:
            view.stop()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

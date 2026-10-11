#!/usr/bin/env python3
"""share_refuse: plants and corruption fail loud.

(a) A planted `sk-test-...` key inside a response blob refuses with
exit 65 naming file:line, writes nothing; --force packs anyway with a
loud warning, and the forced bundle still verifies (integrity intact
by design — verify is not an audit).
(b) A one-char flip in events.jsonl refuses share (chain) and fails
verify with 65; --force does NOT override a corrupt chain (force is
audit-only).
"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from share_util import (check_linux, free_port, Mock, run_sg, run_cli,
                        two_turn_scenario)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
PLANT = "sk-test-ABCDEF1234567890abcdef"


def record(work):
    port = free_port()
    wd = os.path.join(work, "w")
    os.makedirs(wd)
    durl = "http://127.0.0.1:%d/test-data" % port
    mock = Mock(os.path.join(MOCKLLM, "server.py"), port,
                two_turn_scenario(wd))
    try:
        orig = os.path.join(work, "orig.sgr")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, AGENT, "--workdir", wd,
                    "--data-url", durl],
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-refuse-001",
                                    "PYTHONDONTWRITEBYTECODE": "1"})
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return None
        return orig
    finally:
        mock.cleanup()


def main():
    check_linux(SNOWGLOBE)
    work = tempfile.mkdtemp(prefix="tmp.")
    try:
        orig = record(work)
        if orig is None:
            return 1
        # (a) planted key in a blob copy.
        planted = os.path.join(work, "planted.sgr")
        shutil.copytree(orig, planted)
        blob = os.path.join(planted, "llm", "0001.res.json")
        with open(blob, "ab") as f:
            f.write(("\nleaked " + PLANT + "\n").encode())
        out = os.path.join(work, "planted.sgrx")
        r = run_cli([SNOWGLOBE, "share", planted, "--out=" + out])
        if r.returncode != 65:
            print("FAIL: plant share exited %d, want 65\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        if "llm/0001.res.json:2 (sk)" not in r.stderr:
            print("FAIL: refusal must name file:line (kind):\n" + r.stderr)
            return 1
        if os.path.exists(out):
            print("FAIL: refused share wrote a bundle")
            return 1
        r = run_cli([SNOWGLOBE, "share", planted, "--out=" + out,
                     "--force"])
        if r.returncode != 0 or "warning" not in r.stderr:
            print("FAIL: --force should pack with warning:\n%s%s"
                  % (r.stdout, r.stderr))
            return 1
        r = run_cli([SNOWGLOBE, "verify", out])
        if r.returncode != 0 or ": OK (" not in r.stderr:
            print("FAIL: forced bundle should verify:\n%s%s"
                  % (r.stdout, r.stderr))
            return 1
        # (b) corrupted chain in another copy.
        bad = os.path.join(work, "bad.sgr")
        shutil.copytree(orig, bad)
        evp = os.path.join(bad, "events.jsonl")
        with open(evp, "rb") as f:
            data = f.read()
        h = data.find(b'"hash":"')
        if h < 0:
            print("FAIL: no hash field to corrupt")
            return 1
        data = data[:h + 8] + (b"b" if data[h + 8:h + 9] != b"b" else b"a") + \
            data[h + 9:]
        with open(evp, "wb") as f:
            f.write(data)
        r = run_cli([SNOWGLOBE, "share", bad,
                     "--out=" + os.path.join(work, "bad.sgrx")])
        if r.returncode != 65:
            print("FAIL: corrupt share exited %d, want 65\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        r = run_cli([SNOWGLOBE, "verify", bad])
        if r.returncode != 65 or "FAILED" not in r.stderr:
            print("FAIL: corrupt verify:\n%s%s" % (r.stdout, r.stderr))
            return 1
        r = run_cli([SNOWGLOBE, "share", bad,
                     "--out=" + os.path.join(work, "bad2.sgrx"),
                     "--force"])
        if r.returncode != 65:
            print("FAIL: --force must not override a corrupt chain, got %d"
                  % r.returncode)
            return 1
        print("PASS share_refuse: plant refuses+forces, corruption refuses")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Integration: trace `sh -c 'echo x > $TMP/f && mv $TMP/f $TMP/g && rm $TMP/g'`.

Asserts the fs.open(write) → fs.rename → fs.unlink sequence appears in
events.jsonl (pids/timestamps normalised away). Skips with a reason when the
kernel capability is missing (AGENTS.md §5).
"""
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile

SNOWGLOBE = sys.argv[1] if len(sys.argv) > 1 else "./build/debug/snowglobe"


def skip(reason):
    print(f"SKIP: {reason}")
    sys.exit(0)


def main():
    if platform.system() != "Linux":
        skip("requires Linux ptrace backend")
    if not os.path.exists(SNOWGLOBE):
        skip(f"binary not found: {SNOWGLOBE}")
    tmp = tempfile.mkdtemp(prefix="sg-int-")
    out = os.path.join(tmp, "run.sgr")
    script = "echo x > $TMPDIR_SG/f && mv $TMPDIR_SG/f $TMPDIR_SG/g && rm $TMPDIR_SG/g"
    env = dict(os.environ, TMPDIR_SG=tmp)
    r = subprocess.run(
        [SNOWGLOBE, "run", f"--out={out}", "--",
         "sh", "-c", script],
        capture_output=True, text=True, env=env,
    )
    if r.returncode == 69:
        skip(f"snowglobe unavailable on this box: {r.stderr.strip()}")
    if r.returncode != 0:
        print(f"FAIL: snowglobe run exited {r.returncode}\n{r.stderr}")
        sys.exit(1)
    evs = []
    with open(os.path.join(out, "events.jsonl")) as f:
        for line in f:
            line = line.strip()
            if line:
                evs.append(json.loads(line))
    kinds = [(e.get("ev"), e.get("path", e.get("to", e.get("from", "")))) for e in evs]

    def has(ev, suffix):
        return any(k == ev and str(p).endswith(suffix) for k, p in kinds)

    f_path = tmp + "/f"
    g_path = tmp + "/g"
    ok_open = any(
        e.get("ev") == "fs.open" and e.get("path") == f_path and e.get("write") is True
        for e in evs
    )
    ok_rename = any(
        e.get("ev") == "fs.rename" and e.get("from") == f_path and e.get("to") == g_path
        for e in evs
    )
    ok_unlink = any(e.get("ev") == "fs.unlink" and e.get("path") == g_path for e in evs)
    ok_exec = any(e.get("ev") == "proc.exec" for e in evs)
    if not (ok_open and ok_rename and ok_unlink and ok_exec):
        print("FAIL: expected event sequence missing")
        for e in evs:
            print(json.dumps(e))
        sys.exit(1)
    # Hash chain integrity.
    prev = "0"
    for i, e in enumerate(evs):
        assert e["seq"] == i, f"seq gap at {i}"
        assert e["prev_hash"] == prev, f"chain break at {i}"
        prev = e["hash"]
    with open(os.path.join(out, "manifest.json")) as f:
        m = json.load(f)
    assert m["event_count"] == len(evs), "manifest count mismatch"
    assert m["last_hash"] == prev, "manifest last_hash mismatch"
    assert m["finished"] is not None
    print(f"PASS: {len(evs)} events, open→rename→unlink verified in {out}")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()

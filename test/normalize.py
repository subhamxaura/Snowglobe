#!/usr/bin/env python3
"""Normalize a snowglobe events.jsonl for golden comparison.

Rules (Phase 1A Block 2):
- pid/tid/ppid replaced by stable ids of first appearance (P1, P2, ... /
  T1, T2, ...). ppid maps through the pid namespace (the supervisor pid,
  which never has its own event, still gets a stable id).
- stripped keys: seq, ts_us, t_ms, t_us, prev_hash, hash, fd.
- every string has the scenario temp dir replaced with $TMP, the repo path
  with $REPO, and interpreter-version path components normalised
  (python3.V, cpython-3V) so one golden serves 22.04 (3.10) and 24.04 (3.12).
- output: one canonical JSON object per line (sorted keys).

Usage: normalize.py <events.jsonl> --repo <repo-path>
"""
import json
import re
import sys

STRIP = {"seq", "ts_us", "t_ms", "t_us", "prev_hash", "hash", "fd"}
TMP_RE = re.compile(r"/tmp/tmp\.[A-Za-z0-9]+")
PYVER_RE = re.compile(r"python3\.\d+")
CPYTHON_RE = re.compile(r"cpython-3\d+")


def main():
    args = sys.argv[1:]
    repo = None
    if "--repo" in args:
        i = args.index("--repo")
        repo = args[i + 1]
        del args[i : i + 2]
    if not args or repo is None:
        print("usage: normalize.py <events.jsonl> --repo <repo-path>")
        return 2
    pid_map = {}
    tid_map = {}

    def pid(v):
        if not isinstance(v, int):
            return v
        if v not in pid_map:
            pid_map[v] = "P%d" % (len(pid_map) + 1)
        return pid_map[v]

    def tid(v):
        if not isinstance(v, int):
            return v
        if v not in tid_map:
            tid_map[v] = "T%d" % (len(tid_map) + 1)
        return tid_map[v]

    def fix(o):
        if isinstance(o, dict):
            r = {}
            for k, v in o.items():
                if k in STRIP:
                    continue
                if k == "pid":
                    r[k] = pid(v)
                elif k == "tid":
                    r[k] = tid(v)
                elif k == "ppid":
                    r[k] = pid(v)
                else:
                    r[k] = fix(v)
            return r
        if isinstance(o, list):
            return [fix(x) for x in o]
        if isinstance(o, str):
            o = TMP_RE.sub("$TMP", o)
            o = o.replace(repo, "$REPO")
            o = PYVER_RE.sub("python3.V", o)
            o = CPYTHON_RE.sub("cpython-3V", o)
            return o
        return o

    with open(args[0]) as f:
        for line in f:
            line = line.strip()
            if line:
                print(json.dumps(fix(json.loads(line)), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Generate a ~50 MB synthetic events.jsonl for the viewer perf test.

Valid JSON lines with seq/ts/ev/pid/tid (chain fields omitted — the viewer
does not verify them). Deterministic: same seed content every run.
Usage: gen-big-trace.py <dir> <megabytes>
"""
import json
import sys

EVS = [
    ("proc.start", {"ppid": 100, "root": True}),
    ("proc.exec", {"path": "/usr/bin/python3", "argv": ["python3", "x"], "cwd": "/tmp"}),
    ("fs.open", {"path": "/tmp/f", "write": True, "create": True, "trunc": True, "fd": 3}),
    ("fs.open", {"path": "/lib/x86_64-linux-gnu/libc.so.6", "write": False, "create": False,
                 "trunc": False, "fd": 4}),
    ("net.connect", {"addr": "127.0.0.1:80", "ok": True, "initiated": True}),
    ("proc.exit", {"code": 0, "signal": 0}),
]


def main():
    outdir, mb = sys.argv[1], int(sys.argv[2])
    target = mb * 1024 * 1024
    written = 0
    seq = 0
    pid = 1000
    with open(outdir + "/events.jsonl", "w") as f:
        while written < target:
            for ev, fields in EVS:
                line = json.dumps({"seq": seq, "ts_us": 1727347200000000 + seq,
                                   "t_ms": seq, "ev": ev, "pid": pid, "tid": pid,
                                   **fields}) + "\n"
                f.write(line)
                written += len(line)
                seq += 1
                if seq % 997 == 0:
                    pid += 1
    print("wrote %d bytes %d events" % (written, seq))


if __name__ == "__main__":
    main()

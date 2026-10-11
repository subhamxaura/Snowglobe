# backend comparison: ptrace vs notify (Phase 4 Block 2)

date: 2026-10-11T09:48:38Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=fbd2e20
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run --backend=<be>` with identical `bash -c` shape (.sgr event counts are medians); .sgr dirs removed after each run

| workload | untraced med ms (min-max) | ptrace med ms (min-max) | ptrace ratio | ptrace events | notify med ms (min-max) | notify ratio | notify events |
|---|---|---|---|---|---|---|---|
| forkexec-300 | 253.4 (246.2-254.7) | 1736.8 (1715.8-1749.5) | 6.85x | 2123 | 1998.9 (1983.6-2011.9) | 7.89x | 2123 |
| python-import | 59.7 (59.1-66.2) | 148.0 (139.2-152.3) | 2.48x | 144 | 76.0 (74.4-77.0) | 1.27x | 149 |
| git-status | 5.3 (5.3-5.6) | 50.5 (49.0-59.1) | 9.44x | 52 | 19.7 (19.5-19.8) | 3.68x | 73 |
| find-usrlib | 223.5 (222.3-15359.5) | 8142.7 (7965.3-8188.1) | 36.43x | 31 | 536.2 (529.9-560.4) | 2.40x | 31 |

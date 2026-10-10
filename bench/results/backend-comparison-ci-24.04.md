# backend comparison: ptrace vs notify (Phase 4 Block 2)

date: 2026-10-10T05:20:49Z
machine: x86_64 nproc=4 mem_kb=16373448
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=6f08403
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run --backend=<be>` with identical `bash -c` shape (.sgr event counts are medians); .sgr dirs removed after each run

| workload | untraced med ms (min-max) | ptrace med ms (min-max) | ptrace ratio | ptrace events | notify med ms (min-max) | notify ratio | notify events |
|---|---|---|---|---|---|---|---|
| forkexec-300 | 254.3 (249.7-261.1) | 1724.9 (1723.2-1742.4) | 6.78x | 2123 | 2014.4 (1999.6-2029.1) | 7.92x | 2123 |
| python-import | 60.6 (59.1-198.8) | 143.9 (141.3-151.0) | 2.38x | 144 | 77.7 (72.9-83.5) | 1.28x | 149 |
| git-status | 5.3 (5.2-5.4) | 51.1 (47.3-54.3) | 9.72x | 52 | 19.4 (19.3-20.1) | 3.69x | 74 |
| find-usrlib | 227.4 (224.6-13953.6) | 7999.9 (7881.6-8103.8) | 35.18x | 31 | 520.0 (517.0-528.0) | 2.29x | 31 |

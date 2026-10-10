# backend comparison: ptrace vs notify (Phase 4 Block 2)

date: 2026-10-10T10:52:05Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=82f67de
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run --backend=<be>` with identical `bash -c` shape (.sgr event counts are medians); .sgr dirs removed after each run

| workload | untraced med ms (min-max) | ptrace med ms (min-max) | ptrace ratio | ptrace events | notify med ms (min-max) | notify ratio | notify events |
|---|---|---|---|---|---|---|---|
| forkexec-300 | 215.4 (211.9-231.8) | 1507.4 (1478.5-1577.9) | 7.00x | 2123 | 1934.7 (1923.4-2074.4) | 8.98x | 2123 |
| python-import | 45.3 (44.7-206.0) | 117.7 (112.4-121.6) | 2.60x | 144 | 57.2 (55.9-58.5) | 1.26x | 149 |
| git-status | 4.4 (4.4-4.7) | 40.9 (39.8-44.2) | 9.20x | 52 | 17.0 (16.9-17.1) | 3.83x | 73 |
| find-usrlib | 175.3 (171.7-13429.9) | 7187.6 (6969.5-7303.9) | 41.01x | 31 | 446.7 (428.6-455.5) | 2.55x | 31 |

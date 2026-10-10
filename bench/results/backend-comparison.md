# backend comparison: ptrace vs notify (Phase 4 Block 2)

date: 2026-10-10T04:30:03Z
machine: x86_64 nproc=16 mem_kb=8036548
kernel: 6.6.87.2-microsoft-standard-WSL2
uid: 1000
snowglobe: snowglobe 0.1.0-phase0
repo: /home/tester/src/snowglobe commit=b9dc27c
repo_dirty_files: 30
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run --backend=<be>` with identical `bash -c` shape (.sgr event counts are medians); .sgr dirs removed after each run

| workload | untraced med ms (min-max) | ptrace med ms (min-max) | ptrace ratio | ptrace events | notify med ms (min-max) | notify ratio | notify events |
|---|---|---|---|---|---|---|---|
| forkexec-300 | 399.2 (385.5-413.5) | 5430.7 (5324.7-5505.2) | 13.60x | 2116 | 2719.8 (2702.2-2812.2) | 6.81x | 2116 |
| python-import | 88.8 (87.3-90.8) | 351.8 (343.4-359.2) | 3.96x | 137 | 115.0 (113.5-118.8) | 1.29x | 142 |
| git-status | 11.9 (11.3-12.7) | 165.6 (162.7-171.6) | 13.96x | 50 | 40.5 (39.8-42.3) | 3.41x | 89 |
| find-usrlib | 2958.1 (2810.1-3086.1) | 7825.3 (5996.4-8314.3) | 2.65x | 24 | 3224.9 (2952.7-3451.1) | 1.09x | 24 |

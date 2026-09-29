# ptrace baseline (Phase 1A Block 3)

date: 2026-09-29T15:01:00Z
machine: x86_64 nproc=4 mem_kb=16372440
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=99d1ae0
repo_dirty_files: 2
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 220.4 (219.8-225.6) | 2691.3 (2646.5-2702.8) | 2618.9 (2506.7-2842.5) | 2398.5 | 11.88x | 2123 |
| python-import | 69.6 (66.8-79.5) | 188.2 (173.2-230.9) | 191.4 (168.9-221.5) | 121.9 | 2.75x | 144 |
| git-status | 4.2 (4.1-4.3) | 61.0 (55.9-80.9) | 61.9 (57.9-81.8) | 57.7 | 14.80x | 52 |
| find-usrlib | 137.5 (136.9-19289.6) | 12129.1 (11679.3-12560.7) | 11609.7 (11065.2-11844.1) | 11472.3 | 84.46x | 31 |

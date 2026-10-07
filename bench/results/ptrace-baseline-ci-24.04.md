# ptrace baseline (Phase 1A Block 3)

date: 2026-10-07T17:30:43Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=b16ed78
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 303.8 (296.0-308.1) | 2597.8 (2558.3-2667.6) | 2389.5 (2382.8-2414.1) | 2085.7 | 7.87x | 2123 |
| python-import | 76.4 (75.3-199.5) | 198.7 (192.9-214.0) | 204.3 (190.6-218.1) | 127.8 | 2.67x | 144 |
| git-status | 6.2 (6.2-6.4) | 71.1 (62.8-74.6) | 62.7 (58.9-74.5) | 56.5 | 10.16x | 52 |
| find-usrlib | 241.4 (237.2-33205.9) | 13768.4 (13516.2-13852.0) | 12403.8 (12076.0-12599.9) | 12162.4 | 51.38x | 31 |

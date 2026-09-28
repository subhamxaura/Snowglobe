# ptrace baseline (Phase 1A Block 3)

date: 2026-09-28T15:06:21Z
machine: x86_64 nproc=16 mem_kb=8036544
kernel: 6.6.87.2-microsoft-standard-WSL2
uid: 1000
snowglobe: snowglobe 0.1.0-phase0
repo: /home/tester/src/snowglobe commit=815ebdf
repo_dirty_files: 7
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 471.6 (454.7-498.1) | 6768.7 (6454.8-7004.4) | 5967.7 (5872.8-6165.0) | 5496.0 | 12.65x | 2116 |
| python-import | 90.7 (87.1-96.2) | 430.8 (416.8-463.3) | 402.2 (383.1-421.7) | 311.5 | 4.43x | 137 |
| git-status | 9.5 (8.3-9.7) | 129.8 (128.6-135.1) | 121.6 (120.2-125.2) | 112.1 | 12.86x | 43 |
| find-usrlib | 3181.1 (3130.5-3788.9) | 9196.4 (9190.9-9426.6) | 8673.0 (8554.2-8783.6) | 5491.9 | 2.73x | 24 |

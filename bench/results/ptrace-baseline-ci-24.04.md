# ptrace baseline (Phase 1A Block 3)

date: 2026-09-30T15:03:51Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=95959bc
repo_dirty_files: 2
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 222.3 (215.2-226.7) | 1658.8 (1630.4-1797.7) | 1593.4 (1507.8-1628.6) | 1371.1 | 7.17x | 2123 |
| python-import | 48.3 (47.8-126.9) | 127.8 (120.9-131.8) | 123.5 (118.4-128.6) | 75.2 | 2.56x | 144 |
| git-status | 4.3 (4.1-4.7) | 40.2 (37.2-41.1) | 35.7 (35.5-36.4) | 31.4 | 8.32x | 52 |
| find-usrlib | 176.7 (173.5-12020.4) | 8050.5 (7942.7-8240.4) | 7261.3 (7197.0-7368.4) | 7084.7 | 41.11x | 31 |

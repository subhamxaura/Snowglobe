# ptrace baseline (Phase 1A Block 3)

date: 2026-10-03T19:33:46Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=1cbceac
repo_dirty_files: 2
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 236.4 (228.2-242.3) | 1788.1 (1719.9-1880.6) | 1640.1 (1613.2-1692.1) | 1403.7 | 6.94x | 2123 |
| python-import | 56.7 (55.1-145.2) | 136.5 (129.8-155.8) | 135.3 (126.7-182.1) | 78.6 | 2.39x | 144 |
| git-status | 4.9 (4.7-4.9) | 44.5 (42.0-48.2) | 40.9 (38.5-41.8) | 36.0 | 8.39x | 52 |
| find-usrlib | 185.1 (176.6-12189.1) | 8317.9 (7952.6-8351.5) | 7438.6 (7369.4-7549.7) | 7253.5 | 40.19x | 31 |

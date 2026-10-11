# ptrace baseline (Phase 1A Block 3)

date: 2026-10-11T09:49:07Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=fbd2e20
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 309.4 (302.4-315.2) | 2486.8 (2426.6-2505.9) | 2234.9 (2185.3-2257.7) | 1925.4 | 7.22x | 2123 |
| python-import | 79.9 (77.6-94.2) | 202.9 (195.3-221.8) | 201.3 (179.8-211.4) | 121.4 | 2.52x | 144 |
| git-status | 6.7 (6.7-6.8) | 71.8 (66.1-73.7) | 67.1 (65.0-76.9) | 60.4 | 10.03x | 52 |
| find-usrlib | 242.4 (237.2-13140.0) | 13477.9 (13425.8-13806.9) | 12272.7 (11877.4-12426.4) | 12030.3 | 50.63x | 31 |

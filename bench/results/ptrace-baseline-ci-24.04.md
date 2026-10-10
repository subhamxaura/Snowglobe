# ptrace baseline (Phase 1A Block 3)

date: 2026-10-10T10:52:47Z
machine: x86_64 nproc=4 mem_kb=16373444
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=82f67de
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 309.1 (302.7-317.0) | 2534.5 (2447.7-2563.3) | 2282.3 (2237.2-2330.2) | 1973.2 | 7.38x | 2123 |
| python-import | 80.6 (79.8-92.5) | 218.9 (201.3-242.3) | 208.1 (199.5-232.6) | 127.6 | 2.58x | 144 |
| git-status | 6.6 (6.4-6.7) | 72.1 (66.8-81.0) | 61.0 (59.3-65.8) | 54.4 | 9.28x | 52 |
| find-usrlib | 243.2 (239.8-10759.7) | 13769.4 (13648.9-13857.0) | 12253.4 (11950.2-12433.8) | 12010.2 | 50.39x | 31 |

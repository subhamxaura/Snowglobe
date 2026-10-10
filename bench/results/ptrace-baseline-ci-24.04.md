# ptrace baseline (Phase 1A Block 3)

date: 2026-10-10T04:59:49Z
machine: x86_64 nproc=4 mem_kb=16372440
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=b434ef7
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 231.3 (229.0-234.8) | 2447.9 (2435.8-2575.0) | 2415.0 (2369.1-2454.2) | 2183.7 | 10.44x | 2123 |
| python-import | 73.1 (71.6-199.3) | 187.6 (180.5-198.7) | 190.1 (178.8-214.3) | 117.0 | 2.60x | 144 |
| git-status | 5.1 (5.0-5.1) | 78.2 (70.6-81.2) | 73.5 (67.6-83.2) | 68.4 | 14.54x | 52 |
| find-usrlib | 141.3 (139.7-11641.1) | 10731.3 (10542.9-10996.8) | 9943.2 (9525.8-10004.2) | 9801.9 | 70.37x | 31 |

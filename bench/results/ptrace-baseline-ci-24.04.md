# ptrace baseline (Phase 1A Block 3)

date: 2026-10-10T13:02:07Z
machine: x86_64 nproc=4 mem_kb=16373444
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=ab38425
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 301.1 (298.7-305.7) | 2453.3 (2430.7-2478.0) | 2223.9 (2196.2-2275.7) | 1922.8 | 7.39x | 2123 |
| python-import | 83.0 (74.8-93.3) | 200.0 (185.0-207.2) | 204.1 (198.5-211.6) | 121.2 | 2.46x | 144 |
| git-status | 6.6 (6.5-6.7) | 70.4 (66.0-76.8) | 65.2 (63.9-71.5) | 58.6 | 9.82x | 52 |
| find-usrlib | 236.3 (234.6-11136.1) | 13365.7 (13127.9-13467.4) | 12018.2 (11795.2-12224.4) | 11781.8 | 50.85x | 31 |

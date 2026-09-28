# ptrace baseline (Phase 1A Block 3)

date: 2026-09-28T15:59:39Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=56a2a01
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 238.4 (232.7-240.0) | 1771.4 (1725.6-1786.3) | 1638.8 (1627.2-1655.5) | 1400.4 | 6.87x | 2123 |
| python-import | 52.5 (51.4-60.6) | 133.7 (126.7-139.6) | 129.3 (124.8-131.0) | 76.8 | 2.46x | 144 |
| git-status | 4.6 (4.5-4.6) | 37.5 (36.9-39.4) | 37.6 (35.3-37.9) | 33.0 | 8.20x | 51 |
| find-usrlib | 176.8 (173.8-17365.6) | 8276.1 (7831.2-8682.2) | 7534.3 (7271.3-7912.6) | 7357.4 | 42.61x | 31 |

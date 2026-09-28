# ptrace baseline (Phase 1A Block 3)

date: 2026-09-28T17:52:00Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=a08f759
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 322.2 (321.1-331.6) | 2393.3 (2363.9-2428.5) | 2194.8 (2163.3-2204.7) | 1872.6 | 6.81x | 2123 |
| python-import | 77.4 (76.9-87.8) | 194.3 (188.4-195.8) | 189.3 (188.0-191.4) | 111.9 | 2.45x | 144 |
| git-status | 6.0 (6.0-6.1) | 52.2 (49.9-53.9) | 49.8 (47.2-51.6) | 43.7 | 8.24x | 51 |
| find-usrlib | 285.6 (284.4-11900.1) | 11478.6 (11337.2-11525.4) | 10284.9 (10231.6-10460.6) | 9999.3 | 36.01x | 31 |

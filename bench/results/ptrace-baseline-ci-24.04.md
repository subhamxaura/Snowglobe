# ptrace baseline (Phase 1A Block 3)

date: 2026-10-09T19:06:14Z
machine: x86_64 nproc=4 mem_kb=16373448
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=be64467
repo_dirty_files: 1
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 249.0 (245.2-255.3) | 1961.0 (1887.0-2057.5) | 1769.7 (1699.7-1802.0) | 1520.7 | 7.11x | 2123 |
| python-import | 59.9 (59.2-67.2) | 153.7 (148.4-156.2) | 151.1 (143.0-152.7) | 91.2 | 2.52x | 144 |
| git-status | 5.3 (5.2-5.5) | 53.6 (53.2-61.0) | 47.6 (47.5-54.1) | 42.3 | 9.02x | 52 |
| find-usrlib | 224.8 (224.1-10585.8) | 9075.8 (9044.9-9238.9) | 8157.8 (8092.6-8279.3) | 7933.0 | 36.29x | 31 |

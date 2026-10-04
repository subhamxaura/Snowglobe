# ptrace baseline (Phase 1A Block 3)

date: 2026-10-04T18:48:06Z
machine: x86_64 nproc=4 mem_kb=16372440
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=6bf1c2a
repo_dirty_files: 2
strace: strace -- version 6.8
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape (.sgr event counts are medians); strace runs use `strace -f -qq -o /dev/null` as the reference tracer; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |
|---|---|---|---|---|---|---|
| forkexec-300 | 203.6 (191.1-214.4) | 2711.3 (2630.4-2792.6) | 2439.0 (2180.6-2483.0) | 2235.4 | 11.98x | 2123 |
| python-import | 62.0 (60.9-152.9) | 189.9 (185.2-194.7) | 170.8 (141.5-205.5) | 108.8 | 2.76x | 144 |
| git-status | 4.2 (3.8-4.4) | 92.8 (59.6-106.0) | 57.5 (45.1-74.4) | 53.3 | 13.60x | 52 |
| find-usrlib | 123.8 (114.5-11678.6) | 10450.3 (10387.5-10657.6) | 9679.6 (9451.6-9773.6) | 9555.9 | 78.19x | 31 |

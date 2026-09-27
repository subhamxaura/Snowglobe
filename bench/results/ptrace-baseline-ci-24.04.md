# ptrace baseline (Phase 1A Block 3)

date: 2026-09-27T16:13:29Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=903040b
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | traced med ms (min-max) | overhead ms | ratio |
|---|---|---|---|---|
| forkexec-300 | 303.0 (297.0-303.9) | 2221.4 (2177.2-2253.9) | 1918.4 | 7.33x |
| python-import | 75.4 (74.7-86.4) | 198.9 (186.7-207.2) | 123.6 | 2.64x |
| git-status | 5.5 (5.4-5.6) | 45.1 (44.7-50.6) | 39.6 | 8.21x |
| find-usrlib | 237.0 (235.3-11460.8) | 11993.9 (11929.4-12136.9) | 11756.9 | 50.62x |

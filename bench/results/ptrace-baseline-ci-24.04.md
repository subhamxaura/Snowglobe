# ptrace baseline (Phase 1A Block 3)

date: 2026-09-27T10:08:40Z
machine: x86_64 nproc=4 mem_kb=16372440
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=31d2638
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | traced med ms (min-max) | overhead ms | ratio |
|---|---|---|---|---|
| forkexec-300 | 226.9 (220.4-234.4) | 2460.0 (2416.6-2509.9) | 2233.1 | 10.84x |
| python-import | 70.5 (69.2-163.8) | 199.2 (180.3-212.0) | 128.7 | 2.83x |
| git-status | 4.1 (4.0-4.2) | 46.4 (43.7-58.8) | 42.3 | 11.31x |
| find-usrlib | 145.0 (139.0-21267.5) | 10193.3 (9829.1-10388.7) | 10048.3 | 70.29x |

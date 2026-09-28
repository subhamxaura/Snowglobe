# ptrace baseline (Phase 1A Block 3)

date: 2026-09-28T13:18:36Z
machine: x86_64 nproc=4 mem_kb=16373448
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=7eb2e7e
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | traced med ms (min-max) | overhead ms | ratio |
|---|---|---|---|---|
| forkexec-300 | 308.2 (306.4-311.7) | 2295.2 (2271.7-2360.6) | 1986.9 | 7.45x |
| python-import | 78.1 (77.4-91.8) | 216.2 (189.7-221.6) | 138.0 | 2.77x |
| git-status | 5.6 (5.6-5.7) | 45.9 (42.9-52.3) | 40.3 | 8.21x |
| find-usrlib | 239.2 (237.1-12895.5) | 12207.5 (12069.0-12388.3) | 11968.4 | 51.04x |

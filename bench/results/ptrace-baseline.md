# ptrace baseline (Phase 1A Block 3)

date: 2026-09-27T09:00:13Z
machine: x86_64 nproc=16 mem_kb=8036540
kernel: 6.6.87.2-microsoft-standard-WSL2
uid: 1000
snowglobe: snowglobe 0.1.0-phase0
repo: /home/tester/src/snowglobe commit=28665aa
repo_dirty_files: 7
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run` with identical `bash -c` shape; .sgr dirs removed after each run

| workload | untraced med ms (min-max) | traced med ms (min-max) | overhead ms | ratio |
|---|---|---|---|---|
| forkexec-300 | 386.3 (381.8-432.1) | 5325.2 (5224.1-5384.0) | 4939.0 | 13.79x |
| python-import | 80.4 (68.1-81.1) | 352.4 (327.1-371.8) | 272.0 | 4.38x |
| git-status | 8.2 (7.6-8.5) | 96.2 (95.8-96.9) | 88.0 | 11.75x |
| find-usrlib | 2809.1 (2745.2-3147.2) | 7375.1 (7077.8-7689.4) | 4566.0 | 2.63x |

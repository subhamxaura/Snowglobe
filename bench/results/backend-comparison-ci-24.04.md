# backend comparison: ptrace vs notify (Phase 4 Block 2)

date: 2026-10-10T13:01:24Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=ab38425
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run --backend=<be>` with identical `bash -c` shape (.sgr event counts are medians); .sgr dirs removed after each run

| workload | untraced med ms (min-max) | ptrace med ms (min-max) | ptrace ratio | ptrace events | notify med ms (min-max) | notify ratio | notify events |
|---|---|---|---|---|---|---|---|
| forkexec-300 | 219.6 (214.4-228.5) | 1536.8 (1516.3-1566.9) | 7.00x | 2123 | 1935.2 (1924.6-1944.0) | 8.81x | 2123 |
| python-import | 47.2 (45.7-56.7) | 121.2 (112.2-123.8) | 2.57x | 144 | 59.0 (58.1-63.1) | 1.25x | 149 |
| git-status | 4.6 (4.5-5.0) | 45.7 (42.8-48.2) | 9.88x | 52 | 18.0 (17.7-18.6) | 3.89x | 73 |
| find-usrlib | 180.9 (176.9-10347.7) | 7300.5 (7048.2-7366.2) | 40.36x | 31 | 451.0 (427.4-518.2) | 2.49x | 31 |

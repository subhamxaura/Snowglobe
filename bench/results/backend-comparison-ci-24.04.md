# backend comparison: ptrace vs notify (Phase 4 Block 2)

date: 2026-10-10T09:36:33Z
machine: x86_64 nproc=4 mem_kb=16373452
kernel: 6.17.0-1022-azure
uid: 1001
snowglobe: snowglobe 0.1.0-phase0
repo: /home/runner/work/Snowglobe/Snowglobe commit=6acc35e
repo_dirty_files: 1
method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through `snowglobe run --backend=<be>` with identical `bash -c` shape (.sgr event counts are medians); .sgr dirs removed after each run

| workload | untraced med ms (min-max) | ptrace med ms (min-max) | ptrace ratio | ptrace events | notify med ms (min-max) | notify ratio | notify events |
|---|---|---|---|---|---|---|---|
| forkexec-300 | 303.0 (296.2-314.4) | 2219.4 (2187.2-2267.9) | 7.33x | 2123 | 2178.5 (2141.5-2237.9) | 7.19x | 2123 |
| python-import | 76.8 (75.7-154.3) | 199.3 (180.8-241.1) | 2.60x | 144 | 97.6 (92.4-100.2) | 1.27x | 149 |
| git-status | 6.3 (6.1-6.4) | 58.3 (58.1-62.2) | 9.32x | 52 | 23.3 (23.1-24.7) | 3.73x | 73 |
| find-usrlib | 245.6 (237.8-13662.3) | 12190.1 (12117.9-12365.1) | 49.64x | 31 | 745.8 (693.6-769.9) | 3.04x | 31 |

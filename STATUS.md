# STATUS — Snowglobe
Updated: 2026-09-27  ·  Phase: 1A Block 3 — IN PROGRESS (local green, CI pending)  ·  Next launch: v0.1.0, week 9

> Block 2 gate closed: CI run
> https://github.com/subhamxaura/Snowglobe/actions/runs/36301575125 — all 6
> jobs green (22.04 + 24.04 × debug + asan-ubsan, cross-aarch64, lint).
> Block 3 (bench + docs + real recordings) is a separate session.

> Phase 0 gate closed 2026-09-27 (CI run 36291693217, all 6 green). This
> session did Phase 1A Block 1 only (process-tree correctness); Blocks 2
> (goldens + kill test) and 3 (bench + docs + real recordings) are separate
> sessions. Schema stays 0 (all trace changes are additive optional fields).

## Done (this phase)
- Repo skeleton: CMake ≥3.25 + Ninja, presets (debug/release/asan-ubsan/tsan),
  clang-format (LLVM/100), clang-tidy, editorconfig, gitignore, Apache-2.0 LICENSE
- `ITracer` + ptrace backend (fork/vfork/clone/exec, path canonicalisation,
  open/unlink/rename/mkdir/connect/sendto/bind decode, default filters, decode_error)
- `JsonlWriter` (SHA-256 chain, per-event flush) + manifest lifecycle
- `snowglobe run|doctor|ls|rm|version` (+ honest not-implemented stubs)
- Unit tests (json escaper, sha256 vectors, hash-chain verify) + integration
  golden test (open→rename→unlink + chain integrity) + 6 probes + CI
- docs: PLAN.md, architecture.md, trace-format.md, adr/0001; README (pre-alpha);
  CHANGELOG.md

## In progress
- Block 3: CI poll for Block 3 push (incl. new tcp_loopback + bench jobs),
  then Phase 1A DONE + tag v0.1.0-alpha.1. Do not start Phase 1B here.

## Verification (WSL2 Ubuntu 24.04, all real output)
- `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`:
  100% (4/4) — 3 unit + tracer_basic golden (open→rename→unlink + chain/manifest)
- `asan-ubsan` preset: build clean, 100% (4/4), zero sanitizer findings
- `snowglobe doctor`: kernel/userns/overlayfs/seccomp-notify/cgroup-v2/ptrace
  yes; landlock/pasta no (WSL2 kernel/userland limits — expected, degrades cleanly)
- `run -- python3 -c "open('/tmp/x','w').write('1')"`: 41 events incl.
  `proc.exec /usr/bin/python3` (full argv) + `fs.open /tmp/x write:true`;
  0 decode_error; manifest event_count/last_hash consistent
- `run -- sh -c 'ls | head -1'`: 2 child proc.exec (/usr/bin/ls, /usr/bin/head),
  both ppid == root pid — PIPE TEST PASSED
- `run -- sh -c 'exit 7'`: snowglobe exits 7 (propagation ✓)
- Kill test: `kill -9` supervisor mid-`sleep 100` → no orphan remains
  (PTRACE_O_EXITKILL verified) — KILL TEST PASSED
- 4 Linux-only bugs fixed (see CHANGELOG Fixed): syscall-info enum off-by-one,
  exec ENTRY caching, JSONL `}}`, environ linkage
- Doctor probes made real (no more guessing): Landlock ABI via ruleset VERSION
  query → **abi 3** (earlier /proc guess said absent — wrong); seccomp-notif via
  GET_ACTION_AVAIL → yes; overlayfs via unshare+uid_map+mount dance → yes;
  apparmor userns knob reported (absent on WSL kernel). Datasets: WSL2 6.6.
- Format clean (`clang-format --dry-run --Werror`), aarch64 cross-build clean,
  CI test step simplified to `ctest --preset ${{ matrix.preset }}`.

## Block 1 verification (WSL2 Ubuntu 24.04, all real output)
- `probe/threads.c` (4 pthreads × file open): 4 `proc.start thread:true` with
  pid=tgid + distinct tids; all 4 `fs.open` attributed to the right tid —
  THREAD-ATTRIBUTION-PASSED.
- `probe/mt_exec.c` (leader execs): worker death reported, one `proc.exec`,
  clean end. Non-leader exec (throwaway): vanished-tid exit + scavenged
  `proc.exec /bin/echo argv=[echo nonleader-exec-ok]`, 0 decode_error.
- Signals: SIGKILL root → exit 137 + finalised manifest; SIGTERM snowglobe →
  root TERM'd, drain, exit 143 + finalised; double Ctrl-C → SIGKILL tree,
  exit 70, partial manifest, no orphans.
- Filters: `find` 12 events default vs 43 with `-a`; `fs.rmdir`, `fs.symlink`,
  `fs.chmod mode=493`, `O_TMPFILE` → `path=/tmp write:true tmpfile:true`.
- vfork smoke: child exit 42, parent resumes, exit 0. `UV_USE_IO_URING=0`
  present in child env.
- ctest debug + asan-ubsan: 100% (5/5 incl. new open-flags unit test), zero
  sanitizer findings; clang-format clean.

## Block 2 verification (all as uid 1000 except builds; WSL2 Ubuntu 24.04)
- 6 goldens (262/28/22/59/23/38 events), byte-stable across repeat runs;
  14/14 debug (2.6 s) and 14/14 asan-ubsan (3.4 s), zero sanitizer findings.
- Kill tests: supervisor SIGKILL → no survivors in 2 s; root SIGKILL →
  exit 137 + finalised manifest; supervisor SIGTERM → exit 143 + finalised.
- CI run 36301575125 all green, incl. 22.04 (C helpers dodge CPython
  startup divergence; helpers build sanitizer-free — LSAN is fatal under
  ptrace — with $HELPERS normalised before $REPO).
- Env notes: drvfs denies chmod/utime to non-root (cmake configure must run
  as root; all test execution as uid 1000); /tmp is cleaned across WSL
  reboots (persistent artifacts live outside /tmp).

## Block 3 verification (all as uid 1000 in ~/src/snowglobe ext4 unless noted)
- tcp_loopback (23 events): binds + blocking connects initiated:true;
  non-blocking to 192.0.2.1:80 initiated:true/ok:false (EINPROGRESS proven);
  closed-port connect initiated:false/ok:false; deterministic across reruns.
- Bench: forkexec-300 13.79×, python-import 4.38×, git-status 11.75×,
  find-usrlib 2.63× (release, WSL2, medians of 5). CI bench job commits
  its own md separately.
- Realistic fixture: `pip download requests`, 1025 normalised events,
  0 decode_error, key-material grep 0. No API keys on the box (WSL, tester,
  Windows all checked); "record real-agent fixtures" issue left for human.
- D1 self-review of core/tracer/ptrace (builder==reviewer here; re-run with
  a second model per Part E): 2 High (argv truncation → truncated:true;
  first-stop death → loud error) + 6 Medium fixed, verified (nested tracer
  errors LOUD with message; 100-arg exec flagged truncated:true).
- ctest debug + asan-ubsan 15/15 (incl. tcp_loopback), zero sanitizer
  findings; clang-format clean.

## Next up (ordered)
1. Phase 1A — tracer hardening (6 golden trees, kill-9 no-orphans, bench publish)
2. Phase 1B — LLM proxy + mock + toy agent
3. Phase 1C — embedded viewer

## Known issues / tech debt (with issue links)
- #7 `--capture-stdio` passthrough only (logs created empty) — Phase 1A
- `net.connect` addr formatting only (family/addr/port split deferred) — Phase 1A
- CLI11/nlohmann/json not yet vendored (hand-rolled JSON; proxy phase adds them) — Phase 1B
- No `poc/tracer.c` found in workspace; tracer written fresh from spec — recorded here

## Capability matrix on the dev box (from `snowglobe doctor`, WSL2 Ubuntu 24.04)
| userns | overlayfs-in-userns | landlock | seccomp-notif | cgroup v2 delegated | pasta |
| ✅ (apparmor knob absent) | ✅ (mounted+verified) | ✅ abi 3 | ✅ | ✅ | ❌ |

## Decisions pending the human
- Create the GitHub repo + push (no remote/auth on this box) — push guide sent;
  paste the Actions run URL; CI green is the last gate before DONE

## Metrics (weekly)
stars · installs · WAU CLIs · runs · shares · interviews done · MRR — all zero (pre-alpha)

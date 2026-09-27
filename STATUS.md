# STATUS — Snowglobe
Updated: 2026-09-27  ·  Phase: 0 — Bootstrap: DONE  ·  Next launch: v0.1.0, week 9

> Gate CLOSED 2026-09-27: CI run
> https://github.com/subhamxaura/Snowglobe/actions/runs/36291693217 — all 6
> jobs green (22.04 debug + asan-ubsan, 24.04 debug + asan-ubsan,
> cross-aarch64, lint). The 22.04 pip failure was fixed via setup-python
> (commit 3449f7d). Phase 1A kickoff is a separate session; do not start it here.

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
- None — Phase 0 DONE. Next: Phase 1A kickoff (separate session).

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

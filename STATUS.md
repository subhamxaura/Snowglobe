# STATUS — Snowglobe
Updated: 2026-09-26  ·  Phase: 0 — Bootstrap: VERIFIED IN WSL2, PENDING PUSH/CI  ·  Next launch: v0.1.0, week 9

> Gate update: tracer/doctor/run lifecycle executed for real on WSL2 Ubuntu
> 24.04 (kernel 6.6.87.2-microsoft-standard-WSL2). Debug + asan-ubsan green,
> doctor/run/kill tests pass (see "Verification" below). Phase 0 is done when
> the push + CI run (all jobs green) is pasted in. No push yet: no remote, no
> GitHub auth on this box — needs the human (repo URL + credentials).

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
- Push to GitHub + CI green (BLOCKED: needs human — repo URL + auth)

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

## Next up (ordered)
1. Phase 1A — tracer hardening (6 golden trees, kill-9 no-orphans, bench publish)
2. Phase 1B — LLM proxy + mock + toy agent
3. Phase 1C — embedded viewer

## Known issues / tech debt (with issue links)
- #7 `--capture-stdio` passthrough only (logs created empty) — Phase 1A
- `net.connect` addr formatting only (family/addr/port split deferred) — Phase 1A
- CLI11/nlohmann/json not yet vendored (hand-rolled JSON; proxy phase adds them) — Phase 1B
- No `poc/tracer.c` found in workspace; tracer written fresh from spec — recorded here

## Capability matrix on the dev box (from `snowglobe doctor`)
| userns | overlayfs-in-userns | landlock | seccomp-notif | cgroup v2 delegated | pasta |
| — (Windows dev box; run in CI/WSL2) | — | — | — | — | — |

## Decisions pending the human
- None

## Metrics (weekly)
stars · installs · WAU CLIs · runs · shares · interviews done · MRR — all zero (pre-alpha)

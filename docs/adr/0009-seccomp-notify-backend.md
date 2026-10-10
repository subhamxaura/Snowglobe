# ADR 0009 — seccomp user-notification tracer backend (Phase 4)

Date: 2026-10-09 · Status: accepted (skeleton) · Phase: 4 Block 1

## Context

ADR-0001 promised a seccomp user-notification backend beside ptrace as the
perf chapter: trap only the syscalls we decode, ALLOW everything boring with
zero stops, and bring `npm install` / `pytest` overhead under 1.15× (ptrace
pays 3–14× wall clock on syscall-dense workloads; see
`bench/results/ptrace-baseline.md`). This ADR specifies the backend's CLI
shape, event semantics, and the invariant its filter install relies on.
It amends the CLI contract (AGENTS.md §2): `run --tracer` gains `notify`
(`seccomp` stays an alias), and `--backend=` is accepted as an alias for
`--tracer=` (one flag, two spellings; `--backend` is canonical in new docs).

Probes: `probe/seccomp_notif.c` verifies `SECCOMP_RET_USER_NOTIF`
availability via `SECCOMP_GET_ACTION_AVAIL` (kernel ≥ 5.11); `snowglobe
doctor` already reports the row. The full notif-fd flow (BLOCK 2) is proven
by test, not assumed — in particular the supervisor-death case below.

## Decision

- **Flag: `run [--backend=ptrace|notify|auto]` (`--tracer=` is an alias;
  `--tracer=seccomp` maps to `notify`). Default stays `ptrace`; `auto`
  resolves to `ptrace` until BLOCK 2 numbers + parity land, then to
  `notify` where the capability probe passes. Explicit `--backend=notify`
  on a kernel without user-notify exits 69 with the probe reason.**
- **Notify mode shape: no `PTRACE_SYSCALL` stops.** Process lifecycle
  (fork/vfork/clone/exec/exit) is observed via `PTRACE_SEIZE` +
  `PTRACE_O_TRACE{FORK,VFORK,CLONE,EXEC}` + `PTRACE_O_EXITKILL` only.
  Observed syscalls (`openat`/`openat2`/`unlinkat`/`renameat`/`mkdirat`/
  `symlinkat`/`chmod`/`fchmodat`/`execve`/`connect`/`sendto`/`bind` and
  the same set under their `at`-less aliases where they exist) are
  delivered via `SECCOMP_RET_USER_NOTIF`; every other syscall is
  `SECCOMP_RET_ALLOW` with zero stops.
- **Notification discipline (BLOCK 2 loop, specified here so review can
  hold it to account):** poll the notification fd alongside the ptrace
  wait loop; `SECCOMP_IOCTL_NOTIF_ID_VALID` before every read; answer
  every notification promptly with `SECCOMP_USER_NOTIF_FLAG_CONTINUE`
  (never hold a blocking syscall hostage); read path arguments with
  `process_vm_readv` exactly as the ptrace backend does today. A
  notification that cannot be answered (invalid id, dead tracee) is
  continued, never retried in a hot loop.
- **Supervisor death must not strand tracees.** Unanswered notifications
  block tracees in the kernel. The design answers every notification
  before doing any slow work (path reads happen after CONTINUE where the
  syscall permits, else bounded and non-blocking), and the filter is
  installed such that supervisor death fails closed without wedging
  children permanently. BLOCK 2 ships a kill test proving no stranded
  tracees under notify (supervisor SIGKILL mid-run); if tracees hang,
  this design is wrong and this ADR is revisited — that is a BLOCK 1
  input, recorded here.
- **Documented semantic delta (the honest part):** user-notify observes
  syscall *entry* only — there are no syscall-exit return values. Events
  that ptrace reports with outcome (`fs.unlink/rmdir/rename/symlink/
  chmod ok`, `net.connect/sendto/bind ok`, `fs.open fd`) carry
  **`result_known:false`** under notify instead of `ok`/`errno`/`fd`.
  The key is additive and optional; trace `schema` stays 0. `run.meta`
  gains `"backend":"ptrace"|"notify"` on both backends (ptrace goldens
  stay byte-identical via the normaliser, which strips the new key like
  it strips pids/timestamps — specified here, implemented in BLOCK 2).
- **Byte-identical ptrace semantics.** The ptrace implementation
  (`core/tracer/ptrace/`) is untouched by this ADR; no existing golden
  may change. Parity is defined as shape-equivalent kinds/paths with
  result unknowns pinned (`goldens-notify/` beside the ptrace goldens).
- **Filter install reuses the isolate path and its invariant (R6):** the
  seccomp filter is installed while the installing thread is the only
  thread (single-threaded pre-exec / pre-fork), so no
  `SECCOMP_FILTER_FLAG_TSYNC` is required; the installer documents this
  invariant at the call site (as the isolate installer already does).
  Going multithreaded before install without adding TSYNC is a bug.
- **Revert is a valid outcome.** If BLOCK 2 numbers miss the 1.15×
  target by far, or the semantic delta proves unacceptable to the
  viewer/parity story, Phase 4 reverts to ptrace-only and records why
  here. No sunk-cost backend ships.

## Consequences

- `ITracer` (`core/tracer/common/itracer.hpp`) is the `SyscallBackend`
  interface both backends implement; `PtraceTracer` keeps byte-identical
  semantics (shared helpers only); `NotifyTracer`
  (`core/tracer/notify/`) landed as a probe-only skeleton in Block 1
  and became real in Block 2 (seize lifecycle + notification loop +
  parity suite below).
- Viewer renders `result_known:false` as an explicit "unknown" badge,
  never as ok (BLOCK 2; Vitest + Playwright pin the unaffected paths).
- Bench (`bench/`) compares backends on 300×cat, python-import,
  git-status, find — 5 runs, medians, absolute times + event counts —
  into `bench/results/backend-comparison.md` and into this ADR's follow-up
  note. No perf claims before pasted numbers.
- `doctor` gains a `notify-backend` probe row (honest yes/no; BLOCK 3).
- docs: architecture (two backends + when each applies), limitations
  (semantic-delta table, TOCTOU unchanged), trace-format (`result_known`,
  `run.meta.backend`), README — all BLOCK 3 with the measured numbers.

## Follow-up: measured numbers (Block 2, 2026-10-10)

Release binary, WSL2 6.6.87 x86_64 (16 cores), 5 runs, medians
(`bench/results/backend-comparison.md`):

| workload | ptrace | notify |
|---|---|---|
| forkexec-300 (300×cat) | 13.60x (2116 ev) | **6.81x** (2116 ev) |
| python-import | 3.96x (137 ev) | **1.29x** (142 ev) |
| git-status | 13.96x (50 ev) | **3.41x** (89 ev) |
| find-usrlib | 2.65x (24 ev) | **1.09x** (24 ev) |

Notify beats ptrace on all four. Two honest notes:

- The first measured run had notify at **65x on forkexec-300**: the
  supervisor polled the listener with a 20 ms timeout, and every
  waitpid event arriving with no concurrent notification (fork-event
  resumes when the child is slow to exec, exit reaps) slept the whole
  quantum. Dropping the cap to 1 ms fixed it (6.81x) — same events,
  same code path. Lesson recorded in-code at the poll call: never let
  a quiet-event wait sleep a scheduler quantum on a tracing hot path.
- The 1.15x target (ADR-0001, npm install/pytest) is met on
  tree-walk-shaped work (find 1.09x) and close on interpreter startup
  (1.29x); tiny workloads (git-status, 12 ms plain) are dominated by
  fixed supervisor startup, not per-syscall cost. No npm-install/pytest
  measurement exists yet — no claim is made beyond this table. The
  revert clause above stays open pending wider workload evidence.

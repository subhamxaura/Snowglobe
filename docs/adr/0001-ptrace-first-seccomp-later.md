# ADR 0001 — ptrace first, seccomp-notify later

Date: 2026-09-26 · Status: accepted · Phase: 0

## Context

Snowglobe needs a syscall tracer that runs unprivileged on stock distro kernels
(≥ 5.11) and produces schema-v0 events. Candidates: ptrace
(`PTRACE_GET_SYSCALL_INFO` + `process_vm_readv`), seccomp user-notification,
eBPF.

## Decision

- **v0.1: ptrace only**, with `PTRACE_O_TRACE{FORK,VFORK,CLONE,EXEC,EXITKILL}`,
  `PTRACE_GET_SYSCALL_INFO`, `process_vm_readv` (AGENTS.md §1.3).
- **v0.4: seccomp user-notification** backend trapping only decoded syscalls,
  behind `--tracer=auto|ptrace|seccomp`, with mandatory parity tests.
- eBPF stays optional and off by default (needs privileges/CO-RE toolchain).

## Consequences

- ptrace overhead is accepted as "measured and published, any value acceptable"
  (AGENTS.md §5); seccomp must hit < 15% on `npm install` / `pytest`.
- The `ITracer` interface (core/tracer/common/itracer.hpp) keeps both backends
  interchangeable; golden fixtures in test/ pin the event stream.

## Overhead (from PLAN.md §15 — to be replaced with measured numbers)

| workload | ptrace (expected) | seccomp-notify (target) |
|---|---|---|
| fork+exec microbench | 10–50× per syscall | 2–5× per trapped syscall |
| `npm install` (40 deps) | 2–5× wall | < 1.15× wall |
| `pytest` (2k tests) | 1.5–3× wall | < 1.15× wall |

Measurement harness: bench/syscall_microbench.sh; results in bench/results/.
Phase 1A must publish the real ptrace column; Phase 4 the seccomp column.

## Probes

- probe/ptrace_scope.c, probe/seccomp_notif.c verify runtime availability.
- `snowglobe doctor` reports both; missing features degrade with EX_UNAVAILABLE.

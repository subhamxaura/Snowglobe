# ADR 0012 — --proc=host opt-in for isolate runs

Date: 2026-10-11 · Status: accepted · Phase: 6 Block 3 (issue #4)

## Context

Fresh procfs mounts inside the isolate user namespace fail EPERM on the
kernels we target (issue #4, probe/proc_pidns.c), so the sandbox `/proc`
is an empty tmpfs and runtimes needing `/proc/self` content (Bun, some
Node) abort at startup — the Phase-2 real-runtime gate never ran for
them. The issue thread specified the workaround (opt-in host rbind,
`--proc=empty|host`, run.meta/manifest record, doctor row, integration
test, threat-model + limitations updates); this ADR accepts that shape
with the flag spelled `--proc=host` (absent = previous behavior).

## Decision

- `run --isolate --proc=host` bind-mounts host `/proc` over the sandbox
  `/proc` with the existing `bindRo` helper (`MS_BIND|MS_REC` + recursive
  read-only remount) instead of the empty tmpfs. Any other `--proc`
  value is a usage error; `--proc` without `--isolate` is a usage error.
- The choice is recorded in `run.meta` (`"proc":"host"`, absent otherwise)
  and in the manifest isolate block (`"proc":"host"|"empty"`).
  Schema stays 0 (additive, isolate-only).
- Every `--proc=host` run prints a stderr warning naming the tradeoff
  (host pids visible) and pointing at limitations.md.
- `doctor --isolate` gains an informational `proc-mode` row: default
  empty-tmpfs (host pids hidden) vs `--proc=host` rbind (sandbox sees
  host pids).
- Threat model: the host-pid invisibility property is explicitly
  opt-out (threat-model.md row updated); limitations.md carries the
  consequence verbatim. Default behavior is byte-identical to before.

## Consequences

- `--proc=host` is for trusted-agent debugging (e.g. the Claude Code
  invalid-key smoke), never the default dry-run. The old empty-/proc
  crash documentation stays accurate for the default path.
- A host rbind is NOT a fresh procfs: `/proc` shows host pids (with
  cmdlines/environ readable by the sandbox user), pid namespaces still
  isolate the process tree itself. No new capability is required beyond
  what `--isolate` already needs (same bind path as `/sys`).

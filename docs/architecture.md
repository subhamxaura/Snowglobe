# Architecture (Phase 0 snapshot)

```
┌──────────┐  fork+PTRACE_TRACEME   ┌──────────────┐
│snowglobe │ ────────────────────► │ agent child  │
│supervisor│ ◄── waitpid / syscall │ process tree │
│          │     info + vm_readv   └──────────────┘
└────┬─────┘
     │  JsonlWriter (hash-chained events.jsonl + manifest.json)
     ▼
  <run>.sgr/
```

- **cli/**: arg parsing, run-dir lifecycle, manifest, epilogue. Zero external
  deps (CLI11 via FetchContent arrives with the proxy in Phase 1B).
- **supervisor**: currently folded into `snowglobe run`; splits out when the
  LLM proxy (Phase 1B) and sandbox (Phase 2) need lifecycle management.
- **tracer/ptrace**: syscall enter/exit via `PTRACE_GET_SYSCALL_INFO`;
  fork/vfork/clone/exec tracked; strings via `process_vm_readv`.
- **trace/**: `JsonlWriter` — per-event flush, SHA-256 chain from day one.
- **doctor**: runtime capability table (userns, overlayfs, landlock,
  seccomp-notify, cgroup v2, pasta/slirp4netns, ptrace scope).

Phase 1B adds core/proxy (epoll + OpenSSL forwarder, zero response buffering)
and supervisor env injection. Phase 2 adds core/sandbox (clone3 namespaces,
overlayfs, seccomp-bpf, landlock, cgroup limits). See docs/PLAN.md.

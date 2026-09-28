# Architecture (Phase 1B snapshot)

```
┌──────────┐  fork+PTRACE_TRACEME   ┌──────────────┐
│snowglobe │ ────────────────────► │ agent child  │
│supervisor│ ◄── waitpid / syscall │ process tree │
│          │     info + vm_readv   └──────────────┘
│          │  ┌─────────────────────────────────┐
│          │  │ LLM proxy (127.0.0.1:ephemeral) │
│          │  │  agent ──► proxy ──► upstream   │
│          │  └─────────────────────────────────┘
└────┬─────┘
     │  JsonlWriter (hash-chained events.jsonl + manifest.json)
     │  + llm/ blobs (raw req/res bytes, per-chunk .idx timing)
     ▼
  <run>.sgr/
```

- **cli/**: arg parsing, run-dir lifecycle, manifest, epilogue. `--upstream
  PROVIDER=URL` overrides, `--no-llm-proxy` opts out of capture entirely.
- **supervisor**: currently folded into `snowglobe run`; splits out when the
  sandbox (Phase 2) needs lifecycle management. Owns proxy startup order:
  proxy listens *before* the child forks, so env injection is inherited.
- **tracer/ptrace**: syscall enter/exit via `PTRACE_GET_SYSCALL_INFO`;
  fork/vfork/clone/exec tracked; strings via `process_vm_readv`.
- **trace/**: `JsonlWriter` — per-event flush, SHA-256 chain from day one.
- **doctor**: runtime capability table (userns, overlayfs, landlock,
  seccomp-notify, cgroup v2, pasta/slirp4netns, ptrace scope).

## LLM proxy (core/proxy, Phase 1B)

Transport decision: cpp-httplib streaming, not a custom epoll forwarder
(see ADR-0004). Data flow per request:

```
agent ──POST /openai/v1/…──► handler thread ──► upstream worker thread
   (ContentReader)                │  raw body buffered; stored envelope
                                  │  written; llm.request emitted
                                  ▼
                        httplib::Client(base) + content_receiver
                                  │  head → headers/CT/ext recorded
                                  │  each body chunk → .idx line + file
                                  │    append + queue push (1 MiB cap,
                                  │    blocking push = backpressure)
                                  ▼
agent ◄── chunked provider ◄── queue pop (zero app buffering)
   (server thread)          │  client gone → abort → truncated:true
                            ▼  worker joined → blobs renamed → llm.response
```

**Routes** (`resolve()`): `/openai[/…]` → `https://api.openai.com` (path
kept), `/anthropic[/…]` → `https://api.anthropic.com`,
`/gemini[/…]` → `https://generativelanguage.googleapis.com`,
`/u/<base64url-upstream>[/…]` → arbitrary http(s) base; anything else is
recorded as provider `unknown` and answered 404. `--upstream PROVIDER=URL`
replaces any default base (tests point providers at the mock).

**Env injection** (overwrites user settings; capture requires our proxy):
`OPENAI_BASE_URL` + `OPENAI_API_BASE` =
`http://127.0.0.1:<port>/openai/v1` (the `/v1` is load-bearing:
OpenAI-conformant SDKs append unversioned `/chat/completions`),
`ANTHROPIC_BASE_URL` + `ANTHROPIC_API_BASE` = `…/anthropic`,
`GOOGLE_GEMINI_BASE_URL` + `GEMINI_API_BASE` = `…/gemini` (those SDKs
version their own paths). (`AZURE_OPENAI_ENDPOINT` / `OLLAMA_HOST` from
the CLI contract are not injected yet — see limitations.)

**Timing capture**: `t_ms` = monotonic ms since proxy start (≈ run start);
per-chunk `.idx` lines `{"off":<bytes so far>,"ts_us":<wall>}` are written
by the upstream receiver as chunks arrive. `ttfb_ms` = first *body* byte
arrival minus request start (head arrival is request RTT, useless for
streaming latency) — measured proxy-side; client-side TTFB is what the
Block 2 latency test pins (1.2 ms debug delta over direct).

**Redaction boundaries** (policy: ADR-0003): forwarding is byte-identical
(`Authorization` et al. pass through untouched); only the *stored* copy is
redacted (headers → `REDACTED`, bodies verbatim). Tracer `proc.exec` argv
and both `run.meta`/`manifest` cmds are redacted via the same module;
env fingerprint is names-only.

**Thread ownership**: httplib pool serves; one upstream worker per
request; per-request `Flight` state is joined before the provider
returns — never detached (except the shutdown-during-head-wait path,
which detaches a `shared_ptr`-kept flight rather than joining a
possibly-stuck connect). The event sink is called from handler threads
and is mutex-guarded by the owner. Bounded queue, explicit abort,
`stop(deadlineS)` drains ≤10 s then aborts stragglers and joins all.

Phase 2 adds core/sandbox (clone3 namespaces, overlayfs, seccomp-bpf,
landlock, cgroup limits). See docs/PLAN.md.

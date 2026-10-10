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
- **tracer/ptrace** (default): syscall enter/exit via `PTRACE_GET_SYSCALL_INFO`;
  fork/vfork/clone/exec tracked; strings via `process_vm_readv`.
- **tracer/notify** (`run --backend=notify`, ADR-0009): lifecycle via
  `PTRACE_SEIZE` + TRACE* events; observed syscalls via
  `SECCOMP_RET_USER_NOTIF` (filter in `tracer/notify/`, default ALLOW).
  The supervisor polls the listener fd alongside waitpid and answers
  every notification with CONTINUE. Entry-only: outcome events carry
  `result_known:false`; `run.meta` carries `backend`. When each applies:
  ptrace is the default (full outcomes, highest overhead); notify is for
  syscall-dense workloads where entry-only visibility suffices (bench:
  notify beats ptrace on all four comparison workloads). `--isolate` +
  notify is rejected (69) until the middle learns the hand-over.
- **trace/**: `JsonlWriter` — per-event flush, SHA-256 chain from day one.
- **doctor**: runtime capability table (userns, overlayfs, landlock,
  seccomp-notify, notify-backend, cgroup v2, pasta/slirp4netns, ptrace scope).

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

## Viewer (Phase 1C, ADR-0005)

Next.js static export (`viewer/`, Pages Router, no dependencies beyond
next/react), built with the project's Node (`npm ci && npm run build`)
and gzip-packed into the binary by `cmake/embed_viewer.py` via
`cmake/embed_viewer.cmake` (build-time, no reconfigure needed; strong
ETags, MIME, immutable bit for hashed assets).
`snowglobe view <run|events.jsonl>` serves the embedded files plus trace
APIs on 127.0.0.1 only (default 7777, `--port=0` for ephemeral, `--open`
via best-effort xdg-open that never fails without it):

- `GET /api/manifest` — raw manifest.json (synthesized for bare `.jsonl`)
- `GET /api/events?from=&to=` — seq-indexed slice, ≤5000/page (400
  beyond, 416 past the end); server slices lines, never re-parses
- `GET /api/blob/<path>` — blobs confined to the run dir (lexical `..`
  reject + canonical symlink containment; escapes and missing files are
  both 404); `/trace/*` stays as a compat alias
- `GET /api/summary` — counts by kind, turns (probes excluded, errors
  included), error turns, processes, tcp/unix hosts + disconnects, files
  written/deleted/renamed, duration; computed once at startup
- `GET /api/links` — `links.json` when the run has one (read per
  request, no ETag: the sidecar may appear while serving), else 404 and
  the viewer falls back to the seq heuristic visibly

Same-origin fetches only — no CDN, fonts, telemetry, or external calls
(asserted in Playwright by failing any non-localhost request).
`viewer/lib/model.ts` owns the schema-0 types + turn layer
(`getTurnForEvent(seq)` is the stable causal interface; probes with no
model and no body never form turns). Provider parsing
(OpenAI/Anthropic full + SSE-delta folding, static cost table) lives in
`viewer/lib/providers.ts` with Vitest fixtures (Block 2 adds the
providers/ split + pricing.json); 50 MB traces page through `/api/events`
with a virtualized list (< 3 s budget, Playwright-measured).

## Linker (Phase 1D, ADR-0006)

`core/link/` attributes traced side effects to LLM turns without
touching the hash-chained events: `snowglobe link <run>` writes the
derived `links.json` sidecar (`--check` verifies it is current, exit 3
when stale). Rules — window (response-anchored span), lineage
(background children keep their birth turn), argv-match (tool command
in exec argv, upgrading the other two) — each emit basis + confidence;
probes stay out and anything unclassifiable lands in an explicit
`unattributed` bucket with a reason. Same-input runs are
byte-identical (golden-tested); the viewer consumes the sidecar when
present and labels the seq-heuristic fallback.

Phase 2 adds core/isolate (clone3-or-unshare namespaces, overlayfs `overlay/upper|etc-upper|home-upper|fs-rw-N` native only, seccomp-bpf Docker-parity + arg filtering, landlock, cgroup/prlimit, secret tmpfs masks `~/.ssh|~/.aws|~/.gnupg` + `--allow-path`). ssh-based git remotes need `--allow-path ~/.ssh`. See docs/PLAN.md + ADR-0007.

Phase 3 adds core/diff (ADR-0008): `run --isolate` snapshots a hashed
`baseline.json` first; `diff` derives the pending change set
(upper-vs-baseline classified A/M/D/R, host-reconciled, hunks from a
hand-rolled Hirschberg diff); `apply` validates everything (conflicts +
symlink/`.git` safety) then writes all-or-nothing (exit 65, zero writes,
on abort); `compare` joins two raw sets. The tracer never learns about
any of this — diff/apply/compare read run artifacts + host files only.

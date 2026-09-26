# PLAN.md — Snowglobe build plan (condensed from the Master Prompt Kit)

> Full phasing; AGENTS.md wins on conflicts. Each phase is one task per
> session (90–180 min agent work). Launches at weeks 9 / 15 / 23 / 29 / 36 —
> cut scope, never the launch.

## Phase 0 — Bootstrap (week 1) ← we are here

CMake ≥3.25 + Ninja skeleton, presets (debug/release/asan-ubsan/tsan),
clang-format/tidy, LICENSE, `ITracer` + ptrace module + `JsonlWriter`,
`snowglobe doctor` + probe/ per capability, CI (22.04/24.04 + asan, format
check, aarch64 cross), ADR-0001, README as v0.1 spec, STATUS.md, CHANGELOG.
Acceptance: presets green in CI; `snowglobe doctor` prints the table;
`snowglobe run -- python3 -c "open('/tmp/x','w').write('1')"` emits
proc.exec + fs.open(write=true).

## Phase 1A — Tracer hardening (weeks 2–3)

Fork/clone/thread tree, ppid tracking, fork-race handling, path
canonicalisation, full syscall set decode, default filters + `--all-opens`,
hash-chained JsonlWriter + manifest finalisation, golden fixtures (6 process
trees), kill-9 no-orphans test, bench/syscall_microbench.sh.
Acceptance: green debug + asan-ubsan; 15 real lines under a coding agent or
`npm install`; docs/trace-format.md matches emission exactly.

## Phase 1B — LLM proxy + mock + toy agent (weeks 4–5)

Format-agnostic HTTP/1.1 forwarder (zero response buffering), raw blob store
with header redaction + .sse.idx timing, llm.request/chunk/response events,
env injection, mock LLM (OpenAI + Anthropic, streaming, tool calls),
examples/toy-agent, TTFB test (< 5 ms delta, last-chunk-delayed-2s).
Acceptance: 3 llm.request/response pairs interleaved with proc/fs/net events;
no Authorization bytes on disk; tsan clean.

## Phase 1C — Viewer (weeks 6–7)

Next.js static export embedded in the binary; Timeline/Process/Files/Network/
Turn/Search views; provider parsers (OpenAI + Anthropic, SSE deltas, tool
calls, cost) in TypeScript with fixtures; 50 MB trace < 3 s, 60 fps scroll;
Vitest + Playwright. Acceptance: toy-agent fixture renders 3 turns, file write
linked under turn 2; zero runtime network calls.

## Phase 1D — v0.1.0 (weeks 8–9)

Causal linking (turn attribution + tool_call heuristic), `run` epilogue +
`--json`, ls/rm/version, install.sh + Brew + pipx wrapper, release workflow,
4 agent examples + fixtures, truthful README, launch assets (HN/X/Reddit/blog
outline). Acceptance: fresh Ubuntu 24.04 `install.sh → run → view` < 2 min;
tag v0.1.0.

## Phase 2 — Dry-run sandbox (weeks 10–15)

clone3 namespaces, overlayfs (upper under run/fs/upper) + fuse fallback,
read-only host bind, tmpfs, secret masks + --unmask, NO_NEW_PRIVS, cgroup/prlimit
limits, diff/apply, seccomp-bpf + landlock, --net host|none|proxy-only (pasta),
threat-model.md. Acceptance per sub-task; tag v0.2.0.

## Phase 3 — Replay/CI/SDKs/share (weeks 16–23)

Canonical request hashing, replay proxy modes, divergence policies + exit
3/4, --freeze-time shim, `compare`, replay GitHub Action, Python/TS SDKs,
opt-in `share`. Acceptance: mutated tool → `replay --policy=fail` exits 3 with
readable report; Action comments on PR. Tag v0.3.0.

## Phase 4 — Perf & hardening (weeks 24–29)

seccomp-notify backend + parity tests (default when available), zstd .sgr
container + pack/unpack, libFuzzer targets, bench matrix with SVG charts,
signed musl releases + SBOM + repro check. Acceptance: < 15% overhead;
fuzzers clean 1h; signed v0.4.0.

## Phase 5 — Cloud (weeks 30–36, commercial `cloud/`)

Upload API + viewer + retention/audit, Stripe billing, signed exports +
`verify`, policies/alerts, IaC + ops. Gate: only if Phase-3 traction gates
pass. Acceptance: beta team upload→browse→policy→alert→export→checkout.

## Gates (§11)

- Gate 1 (post-v0.1): real-agent fixtures recorded; else stay in Phase 1.
- Gate 2 (post-v0.3): 10+ weekly active CLIs or 2 design-partner teams;
  else no Phase 5 spend.

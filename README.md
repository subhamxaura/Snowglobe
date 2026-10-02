# ❄️ Snowglobe

![Snowglobe — AI Agent Execution Observatory](docs/assets/snowglobe-hero.png)

<p align="center">
  <strong>Record. Inspect. Diff. Replay.</strong>
</p>

> A Linux-first systems platform for observing, tracing, isolating,
> diffing, and deterministically replaying AI agent execution.

> **Status: pre-alpha, not yet usable.** Phases 0–1B done: `run` (ptrace
> process/file/network tracing + LLM recording proxy with mock-tested
> redaction) + `view` (embedded offline trace viewer) + `doctor` +
> `ls`/`rm`/`version`. No sandbox, no replay yet. See STATUS.md and
> docs/PLAN.md.

**Tagline.** *Langfuse tells you what the model said. Snowglobe tells you what the agent did.*

## What it will be (v0.1 spec)

`snowglobe run --isolate -- claude -p "…"` runs any coding agent inside a sealed,
observable environment; records every LLM call **and** every process/file/network
side effect, causally linked; lets you review the filesystem diff before applying
it; and replays runs deterministically in CI.

## What exists today (Phases 0–1C)

- `snowglobe run -- <command>` — ptrace tracer + LLM recording proxy →
  `<run>.sgr/` (events.jsonl + manifest.json + `llm/` blobs, SHA-256
  chained). Propagates the child's exit code. Base-URL injection captures
  OpenAI/Anthropic/Gemini traffic; stored headers redacted.
- `snowglobe view <run> [--port=7777] [--open]` — embedded offline viewer
  (Next.js static export, no network calls) on localhost: timeline,
  turns (OpenAI/Anthropic parsing + cost), processes, files, network,
  search. Opens 50 MB traces in < 3 s.
- `snowglobe doctor` — kernel/userns/overlayfs/landlock/seccomp-notify/cgroup/
  pasta/ptrace capability table.
- `snowglobe ls | rm <run> | version` — run bookkeeping.
- `test/mockllm/server.py` + `examples/toy-agent/agent.py` — deterministic
  LLM test rig (stdlib only, no API keys).

## Build (Linux; CI is source of truth)

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
./build/debug/snowglobe doctor
./build/debug/snowglobe run -- python3 -c "open('/tmp/x','w').write('1')"
```

Requires CMake ≥ 3.25, Ninja, GCC/Clang with C++20, Python 3 (tests),
Node 18+ (viewer build, embedded at build time; `SNOWGLOBE_VIEWER=OFF`
skips it). Windows/macOS dev boxes: `doctor` reports unavailable; build
in CI or WSL2.

## Trace layout (schema v0)

See docs/trace-format.md. Every event hash-chained from day one; secrets never
stored (env fingerprint = sorted names only, hashed).

## Roadmap

docs/PLAN.md (phases 0→5) · docs/adr/ · docs/architecture.md · CHANGELOG.md

## License

Apache-2.0 (LICENSE). `cloud/` (Phase 5) will be commercial.

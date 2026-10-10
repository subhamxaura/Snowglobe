# ❄️ Snowglobe

![Snowglobe — AI Agent Execution Observatory](docs/assets/snowglobe-hero.png)

<p align="center">
  <strong>Record. Inspect. Diff. Replay.</strong>
</p>

> A Linux-first systems platform for observing, tracing, isolating,
> diffing, and deterministically replaying AI agent execution.

> **Status: pre-alpha, not yet usable.** `run` (ptrace/notify tracing +
> LLM recording proxy with mock-tested redaction) + `view` (embedded
> offline trace viewer) + `doctor` + `ls`/`rm`/`version` + `link` +
> `run --isolate` + `diff`/`apply`/`compare` + `replay`/`replay-proxy`.
> No `share` yet. See STATUS.md and docs/PLAN.md.

**Tagline.** *Langfuse tells you what the model said. Snowglobe tells you what the agent did.*

## What it will be (v0.1 spec)

`snowglobe run --isolate -- claude -p "…"` runs any coding agent inside a sealed,
observable environment; records every LLM call **and** every process/file/network
side effect, causally linked; lets you review the filesystem diff before applying
it; and replays runs deterministically in CI.

## What exists today (run/isolate/link/diff/apply/compare/replay + viewer)

- `snowglobe run -- <command>` — ptrace tracer + LLM recording proxy →
  `<run>.sgr/` (events.jsonl + manifest.json + `llm/` blobs, SHA-256
  chained). Propagates the child's exit code. Base-URL injection captures
  OpenAI/Anthropic/Gemini traffic; stored headers redacted.
- `snowglobe run --backend=notify -- <command>` — seccomp user-notify
  tracer: same kinds/paths, entry-only outcomes (`result_known:false`,
  no `ok`/`errno`/`fd`), lower overhead on syscall-dense work. Default
  stays ptrace; `--isolate` + notify is rejected (69).
- `snowglobe run --isolate -- <command>` — the above inside user+mount+pid
  namespaces (overlayfs project upper, seccomp + Landlock, hashed
  `baseline.json` snapshot for later diffing).
- `snowglobe link <run> [--check]` — causal turn-attribution sidecar.
- `snowglobe diff <run> [--stat] [--patch=FILE]` — pending change set
  from the overlay upper (honest event-only mode without `--isolate`).
- `snowglobe apply <run> [--dry-run] [--yes]` — validated all-or-nothing
  write-back (exit 65, zero writes, on conflict/rejection).
- `snowglobe compare <runA> <runB> [--stat] [--json]` — change-set join.
- `snowglobe replay-proxy <run> [--realtime]` — stub server: serves the
  run's recorded LLM responses offline (502 + log on unrecorded calls).
- `snowglobe replay <run> [--out=DIR] [--realtime] [--ignore=FIELD]... [-- <command override>]` —
  re-executes the recorded command against the stub, records a new run,
  auto-compares (LLM turns, fs, proc.exec argv, net, exit) into
  `replay-report.json`. Exits 0 clean (same failure counts as clean) /
  65 diverged-or-unrecorded / 69 replay impossible / 70 internal.
  Honest wall: non-LLM traffic is not stubbed (docs/limitations.md).
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

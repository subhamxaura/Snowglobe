# Snowglobe — flight recorder and dry-run sandbox for AI agents

> **Status: pre-alpha, not yet usable.** Phase 0 bootstrap: `run` (ptrace process/
> file/network tracing) + `doctor` + `ls`/`rm`/`version` exist. No LLM proxy, no
> sandbox, no viewer yet. See STATUS.md and docs/PLAN.md.

**Tagline.** *Langfuse tells you what the model said. Snowglobe tells you what the agent did.*

## What it will be (v0.1 spec)

`snowglobe run --isolate -- claude -p "…"` runs any coding agent inside a sealed,
observable environment; records every LLM call **and** every process/file/network
side effect, causally linked; lets you review the filesystem diff before applying
it; and replays runs deterministically in CI.

## What exists today (Phase 0)

- `snowglobe run -- <command>` — ptrace tracer → `<run>.sgr/` (events.jsonl +
  manifest.json, SHA-256 chained). Propagates the child's exit code.
- `snowglobe doctor` — kernel/userns/overlayfs/landlock/seccomp-notify/cgroup/
  pasta/ptrace capability table.
- `snowglobe ls | rm <run> | version` — run bookkeeping.
- `test/integration/tracer_basic.py` — golden open→rename→unlink assertion.

## Build (Linux; CI is source of truth)

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
./build/debug/snowglobe doctor
./build/debug/snowglobe run -- python3 -c "open('/tmp/x','w').write('1')"
```

Requires CMake ≥ 3.25, Ninja, GCC/Clang with C++20, Python 3 (tests).
Windows/macOS dev boxes: `doctor` reports unavailable; build in CI or WSL2.

## Trace layout (schema v0)

See docs/trace-format.md. Every event hash-chained from day one; secrets never
stored (env fingerprint = sorted names only, hashed).

## Roadmap

docs/PLAN.md (phases 0→5) · docs/adr/ · docs/architecture.md · CHANGELOG.md

## License

Apache-2.0 (LICENSE). `cloud/` (Phase 5) will be commercial.

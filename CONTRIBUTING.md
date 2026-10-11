# Contributing to Snowglobe

The constitution is [AGENTS.md](AGENTS.md) — this file is the
day-to-day checklist distilled from it. Where they disagree, AGENTS.md
wins; say so if you spot a conflict.

## Build and test (Linux; CI is the source of truth)

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

- Presets: `debug`, `release`, `asan-ubsan`, `tsan`. New code must be
  warning-free under `-Wall -Wextra -Wpedantic -Werror` on all of them
  (plus `-O3` on `release`/cross — the format gate does not catch
  optimizer-only warnings, CI does).
- `bash scripts/check-format.sh` (clang-format, LLVM style, 100 cols)
  must pass before every push.
- Viewer: `npm run test` (vitest), `npx tsc --noEmit`, Playwright via
  `viewer_e2e` where browsers exist.
- Tests run real process trees, a real mock LLM, and the real toy
  agent — no API keys anywhere. A test that cannot run for lack of a
  kernel capability **skips with a reason**; it never fails and never
  silently passes.

## Working protocol

1. Orient: read `STATUS.md`, `AGENTS.md`, and the plan section, then
   restate the task and its acceptance criteria.
2. Investigate before coding: kernel/libc doubts get a 30-line probe
   in `probe/` (kept — probes are documentation), man pages over guesses.
3. Architectural changes get an ADR first (`docs/adr/NNNN-title.md`,
   Nygard format), code second. CLI and trace-schema changes always do.
4. Small verified steps: build, unit-test, run the binary on fixtures,
   show real output after each step. Five small commits beat one large one.
5. Definition of done: code + tests + fixture + docs + CHANGELOG entry +
   real end-to-end output + `STATUS.md` updated + zero new warnings and
   zero sanitizer findings.
6. Report uncertainty plainly, with what would resolve it.

## Hard no's (enforced in review)

No `sudo`/Docker-in-core at runtime; no buffering streamed LLM
responses; no secrets in traces; no provider parsing in C++; no
security claims beyond `docs/threat-model.md`; no README features that
do not exist; no TODOs without an issue link; no swallowed errors; no
force-push to main; one session per working tree.

## Commits

Conventional Commits (`feat:`, `fix:`, `test:`, `docs:`, `ci:`,
`refactor:`). Each commit builds and passes tests. PR descriptions
carry the exact verification commands and their real output.

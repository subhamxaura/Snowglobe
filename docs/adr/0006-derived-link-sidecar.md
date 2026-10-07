# ADR 0006 — links.json is a derived sidecar, not part of the hash chain

Date: 2026-10-05 · Status: accepted · Phase: 1D Block 1

## Context

Phase 1D attributes side effects to LLM turns. The event stream in
`events.jsonl` is hash-chained per event (trace-format.md); rewriting it
to stamp `turn`/`tool_call` would destroy verifiability (any rewrite
invalidates every downstream hash). Attribution is also heuristic — it
must stay re-derivable as the rules improve, without touching evidence.

This ADR also amends the CLI contract (AGENTS.md §2): it adds
`snowglobe link <run> [--check]`.

## Decision

- Attribution output is `links.json`, written next to `events.jsonl`
  (`<run>.sgr/links.json`; bare-file mode: alongside the file). It is
  explicitly **not** covered by the event hash chain and carries no
  hashes of its own.
- `links.json` is regenerable at any time via `snowglobe link <run>`
  (deterministic: same input → byte-identical output, golden-tested).
  Format versioned independently in its own `version` field (starts at
  1); trace `schema` does NOT bump for link-format changes.
- Every attribution carries `basis` (`window` | `lineage` |
  `argv-match`) + `confidence`; anything unclassifiable lands in an
  explicit `unattributed` bucket with a `reason` — never a guess
  presented as fact.
- `snowglobe link <run> --check` regenerates in memory and byte-compares:
  exit 0 when current, exit 3 when stale or missing (mirrors the replay
  divergence code family). Other failures use EX_USAGE=64 /
  EX_UNAVAILABLE=69 / EX_SOFTWARE=70 per AGENTS.md §2.
- Consumers (viewer `getTurnForEvent`, CI gates) treat links.json as a
  cache: absent or stale → fall back to the seq heuristic and say so.

## R1 clarification (2026-10-08, second-model review)

The `window` basis is a **res-partition**, not `[response, next
request)`: turn N owns `[response N, response N+1)`, tail to end of
trace. The implementation (`spanAt` in `core/link/linker.cpp`: last
`resKey <= k`) always behaved this way; the earlier prose was wrong.
An event landing after request N+1 but before response N+1 attributes
to turn N (pinned by `test_link.cpp` "res-partition pins in-flight
events"). No format change: same `basis` vocabulary, same bytes for all
existing fixtures.

## Consequences

- `share` export includes links.json when present (derived data ships
  with the trace, regenerable on receipt either way).
- Viewers must ignore unknown `basis`/`reason`/`confidence` values
  (forward compatibility for new rules).
- Position rule: events sort by `seq` when every event has one,
  otherwise file order (normalised fixtures strip `seq`).

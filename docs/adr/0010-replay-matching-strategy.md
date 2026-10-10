# ADR 0010 — deterministic replay at the LLM layer (Phase 5)

Date: 2026-10-10 · Status: accepted · Phase: 5

## Context

`snowglobe run` records every LLM call as raw bytes (`llm/NNNN.req.json`
+ `NNNN.res.{json,sse,bin}` + `NNNN.res.idx` chunk timing) plus
`llm.request`/`llm.response` events. Replay re-executes the agent against
those recorded responses and diffs observable behavior against the
original. Replay is honest re-execution, never claimed bit-determinism:
agent code, wall-clock, and non-LLM network re-execute for real.

This ADR amends the CLI contract (AGENTS.md §2): it adds
`snowglobe replay-proxy <run> [--realtime]` and
`snowglobe replay <run> [--out=DIR] [--realtime] [--ignore=FIELD]... [-- <command override>]`,
and replaces the Phase-3 sketch codes (0/3/4) with 0/65/69/70.

## Decision

- **Replay proxy mode** (`core/proxy` + `core/replay/replay_store`):
  serves `llm/*` blobs from the original run instead of upstream.
  Upstream is never contacted, so no API key is needed and `--upstream`
  is irrelevant. Response `Content-Type` comes from the blob extension
  (`.sse` → `text/event-stream`, `.json` → `application/json`, else
  `application/octet-stream`); status comes from the recorded
  `llm.response` event. Response headers were never stored — replayed
  responses carry content-type only (documented, not silent: the
  re-served *body* is byte-exact, the envelope is not).
- **Matching** per incoming request, first unused wins, thread-safe:
  (a) primary — sha256 of the *normalized* request (provider + method +
  path + canonical JSON body with volatile keys stripped and volatile
  string patterns scrubbed) matches a recorded request hash;
  (b) fallback — next unused recorded request for the same endpoint
  (provider + method + path) in record order. Method gates both levels:
  a verb change is a different call at the hash AND at the fallback, so
  a GET probe can never borrow a POST turn on the same route (P1). Each
  recorded response is served **once** (consumed); an agent retry of an
  identical request is therefore a MISS, not a free replay — retries
  are observable behavior. Volatile request strings (timestamps) push
  every turn onto the fallback; concurrently-issued identical requests
  may then serve swapped, which the compare's serve-order rule flags
  for distinct turns (P3).
- **MISS is loud, never invented.** No candidate → request appended to
  `<orig-run>/replay.unrecorded.jsonl`, client receives 502
  `{"error":"snowglobe replay: unrecorded call"}`, counters increment.
  The proxy never synthesizes a model response.
- **Streaming** re-serves body bytes sliced at the recorded `.idx`
  `off` boundaries (`.idx` `off` = bytes received *before* that chunk —
  verified against `proxy.cpp`); empty `.idx` (non-streamed,
  header-only) serves the whole body as one chunk. Default is fast (no
  sleeps); `--realtime` sleeps the recorded inter-chunk `ts_us` gaps.
  Re-chunking is byte-exact by construction (concat == file bytes);
  tests assert sha256 equality.
- **Volatile normalization** (request hashing AND event compare) ports
  `test/normalize.py`: strip `seq/ts_us/t_ms/t_us/prev_hash/hash/fd/
  backend`, pid→Pn / tid→Tn by first appearance, numeric ports→PORT,
  `/tmp/tmp.*`→`$TMP`, `127.0.0.1:N`/`[::1]:N`→PORT, python-version
  path components. JSON bodies additionally drop volatile *keys*
  recursively (`id`, `tool_call_id`, `tool_use_id`, `created`,
  `timestamp`, `ts`, `user`, `request_id`, `session_id`) and scrub the
  same string patterns. Timing fields (`ttfb_ms`, `total_ms`) and the
  join key (`replay_of`) are volatile-by-construction and excluded from
  the LLM compare. Anything else that differs is listed in the report,
  never silently dropped. `--ignore FIELD` strips extra event fields.
- **`snowglobe replay`** re-runs the original `run.meta` cmd (or the
  `--` override — required when the recorded cmd is missing, and the
  escape hatch when it carries REDACTED secrets) under the tracer with
  the replay proxy injected, into a NEW run dir whose manifest carries
  `"replay_of":"<abs original>"` (original runs are never modified
  except the unrecorded log). It then auto-compares and writes
  `<new-run>/replay-report.json` (`version:1`, per-category
  `identical|diverged|unrecorded` for `llm/fs/proc/net/exit`, seq-level
  evidence lines, turn counts, exit codes, order flag). `replay` does
  NOT propagate the agent exit code (unlike `run`): the agent exit is
  *compared* — the same nonzero exit is clean ("same failure"), a
  changed exit is divergence.
- **Compare rules.** `llm`: pairwise normalized request-body equality
  through the serve mapping + turn-count match + monotonic serve order;
  any 502 served → `unrecorded`. `fs`: order-insensitive multiset of
  normalized fs events (content changes at the same path are invisible
  without both uppers — stated in limitations). `proc`: normalized
  `proc.exec`(+`exec_failed`) argv multiset. `net`: normalized endpoint
  multisets. `exit`: exit-code equality.
- **Exit codes:** 0 = clean (all identical, incl. same-failure), 65 =
  diverged-or-unrecorded, 69 = replay impossible (no `llm/` blobs /
  no `llm.request` pairs in the original), 70 = internal error.
  `--policy/--fast/--freeze-time` from the old sketch are NOT accepted
  (usage error; frozen time and fuzzy/live policies are future work).
  `--isolate` replay is not supported (the re-execution runs
  unisolated; isolate originals compare on event-observable behavior —
  see limitations).

## Consequences

- Trace `schema` stays 0 (all new keys additive: manifest
  `replay_of`, `llm.response` `replay_of`, report + unrecorded-log
  sidecars). `viewer` shows a Replay badge from manifest/`replay-report.json`.
- The honest wall (`docs/limitations.md`): external non-LLM traffic is
  NOT stubbed (the toy `http_get` hits a live server on replay);
  request-order swaps of distinct bodies with identical multisets are
  flagged via serve-order, not content; non-isolate fs compare is
  path-level, not content-level; replayed response headers are
  content-type-only.
- Second-model review requested for `core/replay/*` + proxy replay
  branch before alpha.8 (ranges in CHANGELOG; builder==reviewer D1 in
  Block 3).

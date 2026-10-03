# The trace viewer

Snowglobe embeds a static Next.js export in the `snowglobe` binary (ADR-0005).
`snowglobe view <run>` serves it on localhost; it makes **zero network calls**
beyond the local server and works offline. This document describes what the
viewer does today and how it is built, tested, and extended.

## Serving model

- `snowglobe view <run>` accepts a run directory (`.sgr/` or any dir with
  `events.jsonl`) or a bare `events.jsonl` path — the latter synthesizes a
  minimal manifest so single-file traces open too.
- Endpoints: `/api/manifest`, `/api/events?from=&to=` (paged, ≤ 5000 per page;
  400 beyond the cap, 416 past the end), `/api/blob/<path>` (confined to the
  run dir: lexical `..` reject plus canonical symlink containment — escapes
  read as 404), `/api/summary`. `/trace/*` remains a blob compat alias.
- Every API and embedded asset sends an ETag and honors `If-None-Match` with
  304s; hashed assets are served `immutable`. Page weight comes from the
  gzip table produced by `cmake/embed_viewer.py` at build time.
- The API mirror of the turn rules lives in `core/view/view.cpp` and must
  stay in sync with `viewer/lib/model.ts` (probe exclusion, error turns).

## UI

`viewer/pages/index.tsx` hosts the tabs, all backed by virtualised lists
(`components/VList.tsx`) so a 50 MB trace opens in < 3 s:

- **Timeline** — all events in seq order.
- **Turns** — one section per LLM turn (request → response): folded text,
  tool calls with arguments, usage/cost when known, timing (ttfb/total,
  chunk count), truncation and error flags. Turns come from
  `TurnIndex.getTurnForEvent` (see below).
- **Processes / Files / Network** — process tree, write/delete/rename events,
  endpoints bucketed tcp/unix/other (structured `family/ip/port/path` fields
  with legacy formatted-addr fallback, `net.disconnect` handled).
- **Search** — client-side filter over loaded events.

## Turn semantics (locked rules, baked from real runs)

- Every non-probe `llm.response` is a turn, **including non-2xx errors**
  (an early build reported "0 LLM turns" for a run that was two 4xx/5xx
  responses — never again).
- Probes never split a span: `HEAD` pre-flights, or model-less requests
  with no body. A model-less POST *with* a body (Gemini carries the model
  in the URL path) is a real turn.
- A turn spans `[its request seq, next turn's request seq)`: side effects
  between response N and request N+1 belong to turn N. Linking is 1D by
  seq for now; the stable interface is `getTurnForEvent(seq)`.

## Providers and pricing (TypeScript only — locked, never C++)

- `viewer/lib/providers.ts` folds OpenAI Chat Completions and Anthropic
  Messages bodies (full or SSE delta streams) into `{text, toolCalls,
  usage, cost}`. Unknown shapes yield empty text and null usage/cost;
  parsers never throw on agent traffic.
- `viewer/lib/pricing.json` is the single price table: $/1M tokens with a
  `verified` date per model (the date the price was checked against the
  provider's official page — sources listed in the file). **n/a is absence,
  never 0**: an unpriced model (e.g. the mock LLM's `mock-model-1`) renders
  `cost —`, it never renders `$0.00`. Tests pin the table shape.
- Adding a model: append an entry with fresh `verified` + source, extend
  the model-ID tests. Adding a provider: new fold function + dispatch in
  `foldBody` + fixture blobs from a real recording.

## Build and embed

- `npm ci && npm run build` in `viewer/` produces the static export
  (`next.config.mjs`: `output: "export"`, `images.unoptimized`).
- CMake (`SNOWGLOBE_BUILD_VIEWER`, default ON) runs `cmake/embed_viewer.py`,
  which gzips every exported file (`mtime=0` for reproducibility) into a
  C table compiled into the binary — MIME, sha256 ETag, immutable flag.
  `SNOWGLOBE_VIEWER=OFF` is kept as an alias; the build degrades loudly
  (warning, viewer omitted) when node/npm are missing.
- No runtime network access: the export contains no external fonts, CDNs,
  or analytics; the e2e suite fails the build on any non-localhost request.

## Testing

- **Vitest** (`viewer/test/`): `model.test.ts` (turn building on the
  toy-agent fixture + synthetic error/probe/disconnect shapes),
  `providers.test.ts` (SSE framing, OpenAI/Anthropic folding on committed
  fixture blobs, pricing table shape).
- **Playwright** (`viewer/e2e/`, chromium): real `snowglobe view` against
  the committed fixture — tab rendering, turn linkage, and the perf test
  (50 MB trace < 3 s). `run-e2e.sh` skips with a printed reason when node,
  browsers, or the binary are absent (never fails silently).
- Both run inside CTest as `viewer_unit` / `viewer_e2e`; the e2e leg runs
  for real in the CI `viewer` job and skip-prints elsewhere.

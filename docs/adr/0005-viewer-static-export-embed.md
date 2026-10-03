# ADR 0005 — Viewer: Next.js static export embedded in the binary

Date: 2026-10-02 · Status: accepted · Phase: 1C Block 1

## Context

`snowglobe view` must work offline on a fresh Linux box with no Node, no
network, and no install step beyond the single binary (AGENTS.md §1.7:
viewer makes no network calls; §1.2: no Node runtime requirement). The
viewer is TypeScript (provider parsing, timeline rendering) while the
server is C++ (localhost only). The two meet at build time, not runtime.

## Decision

- Next.js 14 Pages Router with `output: "export"`: `npm ci && npm run
  build` writes `viewer/out/`, a directory of static files (system fonts
  only, no CDN, telemetry disabled via `.npmrc` + build env).
- CMake drives the build (`SNOWGLOBE_BUILD_VIEWER=ON`, default ON; CI
  installs Node 20 on every native leg) and packs `viewer/out/` into a
  generated header via `cmake/embed_viewer.py` (stdlib only): each file is
  gzip-compressed (`mtime=0`, reproducible), sha256-hashed (strong ETag
  over the stored bytes), and annotated with MIME + immutable bit.
- `snowglobe view` serves the table on 127.0.0.1 only, with
  `Content-Encoding: gzip`, `ETag` (+304), `Cache-Control: immutable` for
  hashed `/_next/static/` assets, `no-cache` for entry HTML, and
  `X-Content-Type-Options: nosniff`. Trace data goes over `/api/*`
  (`manifest`, seq-indexed `events?from&to` ≤5000/page, confined
  `blob/<path>`, `summary`); `/trace/*` stays as a compat alias for blobs.
- No runtime Node: with `SNOWGLOBE_BUILD_VIEWER=OFF` (aarch64 cross job)
  `view` prints a one-line `EX_UNAVAILABLE` instead. Missing node/npm with
  the option ON degrades to OFF with a loud CMake warning, never a
  silent half-viewer.

## Consequences

- Binary grows by roughly the gzipped viewer (~130 KB on 448 KB of
  `viewer/out/` at Block 1; exact before/after in STATUS.md). Acceptable
  for a single-binary tool; Phase 4 static musl builds re-measure.
- Browsers always send `Accept-Encoding: gzip`, so unconditional
  `Content-Encoding: gzip` on embedded assets is safe; `curl` users pass
  `--compressed`. Non-gzip API clients are unaffected (`/api/*` is
  uncompressed JSON).
- MIME map lives in two places (`embed_viewer.py` + `mimeFor()` in
  `view.cpp`): a comment in each points at the other; the e2e suite
  fetches real assets, so a mismatch surfaces as a rendering failure.
- Viewer iteration needs no reconfigure (build-time custom command), but
  a full `npm ci` runs on first configure — npm cache keeps CI cheap.

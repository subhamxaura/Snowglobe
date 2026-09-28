# ADR 0004 — LLM proxy transport: cpp-httplib streaming, not a custom epoll forwarder

Date: 2026-09-28 · Status: accepted · Phase: 1B Block 1 (recorded in Block 3)

## Context

The proxy must forward LLM traffic while recording it: uploads (agent →
proxy → upstream) and downloads (upstream → proxy → agent) for both
plain JSON and SSE streams, with per-chunk arrival timing, TLS to upstream
with verification on, bounded memory/threads, and TSan-clean concurrency —
built by a team with no spare epoll expertise to spend on a hand-rolled
forwarder.

## Decision

Use cpp-httplib (locked allowed dep, pinned v0.20.1) with the three hooks
that together cover both directions the way we need:

- server-side `ContentReader` (request receive),
- client `Request::content_receiver` (response download — the same hook the
  `Get`-with-receiver overloads use internally),
- server chunked content providers (response send).

Uploads are buffered per request (bounded by client behavior; 50 MB
transient worst case — measured in the Block 2 large test). Downloads
stream chunk-by-chunk with zero application buffering: a bounded
`BlockingQueue` (1 MiB, blocking push = backpressure) bridges the
push-model upstream receiver and the pull-model server provider. One
`httplib::Client` per request (no shared-client races), server
`ThreadPool(32)`.

## Consequences

- No custom event loop to own: TLS, keep-alive, chunked framing, and
  timeouts come from a mature library. What we own is thin: routing,
  redaction boundaries, blob layout, event emission (~600 lines).
- Inherited warts: httplib parses the upstream status line with a
  per-thread `std::regex`, whose first-use compile races inside
  libstdc++-11 internals under concurrency (22.04-tsan finding, CI run
  36447650097) — suppressed narrowly in `test/tsan.supp` with evidence;
  libstdc++ 13 is clean.
- Per-request `Client` + thread setup costs ~1 ms (localhost TTFB delta
  1.2 ms debug, 6 ms asan — the Block 2 latency test pins this).
- Upload buffering is the one unbounded-by-design piece: a hostile 1 GB
  request body would transiently allocate 1 GB in the supervisor. Agent
  traffic is bounded in practice; a cap with 413 is future work (no issue
  filed yet — file one before promising it).
- Pinned to httplib's release cadence; bump deliberately (behavioral
  surface: routing, TLS verification defaults, chunked framing).

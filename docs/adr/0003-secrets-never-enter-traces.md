# ADR 0003 — secrets never enter traces

Date: 2026-09-27 · Status: accepted · Phase: 1B Block 1

## Context

Snowglobe records everything an agent does, and `snowglobe share` uploads
traces. API keys routinely appear in three places: HTTP headers
(`Authorization: Bearer …`), command lines / URLs
(`curl -H …`, `https://user:pass@…`, pasted tokens), and the process
environment. A single leaked key in a committed fixture or a shared trace
is a security incident, so redaction must be structural, not advisory.

## Decision

`core/redact/` (used by the proxy, the tracer, and the CLI) enforces:

1. **Stored headers:** values for `authorization`, `x-api-key`, `api-key`,
   `cookie`, `set-cookie`, `proxy-authorization`, `x-goog-api-key`
   (case-insensitive) are stored as `REDACTED`. The *forwarded* request is
   byte-identical to what the agent sent (redaction and forwarding are
   separate code paths; the Block 2 mock asserts both sides).
2. **Tokens in free text** (proc.exec argv, run.meta cmd, URLs, paths):
   URL userinfo (`scheme://user:pass@` → `user:REDACTED@`), key shapes
   (`sk-…` ≥20 chars, `sk-ant-…`, `ghp_/gho_/github_pat_…` ≥20 chars,
   `AKIA`+16, `xox[abp]-…`, `Bearer`/`Basic` + token ≥8 chars),
   `key=`/`token=`/`password=` query params, and any token equal to the
   value of a root-process env var whose name ends in
   KEY|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIAL (values <8 bytes ignored).
3. **Environment fingerprint** stays names-only (AGENTS.md §1.9): sorted
   variable names hashed, values never read.

Length floors (`sk-` ≥20, `Bearer` token ≥8) protect ordinary words, UUIDs,
and git SHAs — all pinned by unit tests that must keep passing. Bare
`pass` is deliberately NOT a sensitive suffix (COMPASS/BYPASS); extend the
set only via a new ADR.

## Consequences

- `share` prints what will be uploaded and requires `--yes`, but the trace
  on disk is already redacted — sharing cannot leak what was never stored.
- Replay-time request hashing (Phase 3) sees redacted argv; canonicalisation
  must treat `REDACTED` as opaque (recorded here so replay does not "fix"
  it back).
- Known gaps (documented, not silently accepted): short `Bearer`/`Basic`
  tokens (<8 chars) are kept as likely prose; non-UTF8 request bodies are base64-wrapped
  with their headers redacted; io_uring traffic is invisible to the tracer
  at all (see `docs/limitations.md`).
- Trace readers must treat redaction as lossy: a `REDACTED` argv cannot be
  replayed against a live provider without re-injection (Phase 3 `live`
  policy).

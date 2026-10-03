# claude-code-1-error — REAL Claude Code error-path fixture (401)

A genuine `claude -p hi` session (request blobs carry User-Agent
`claude-cli/2.1.284`, model `claude-opus-5-5`) recorded
under `snowglobe run` against the **real Anthropic API with an invalid
key**: every real turn came back HTTP 401 with a real Anthropic error
envelope. This is the real-data gate for Block 2 (Anthropic error
parsing) and Block 3 (error-turn rendering).

Provenance: recorded 2026-10-02 18:07–18:10 UTC on WSL2 Ubuntu 24.04
as uid 1000 with the debug build.

Contents: **735 normalised events, 0 `trace.decode_error`**, 12
`llm.request` / `llm.response` pairs, 36 blobs (12 × req/res/idx;
0-byte `*.tmp` transients dropped):

- id 0 — `HEAD /anthropic/api/hello` pre-flight probe (model-less,
  bodyless → NOT a turn; its response is the proxy's 502
  `{"error":"upstream failed"}`).
- ids 1–11 — **11 real turns, all status 401**, body
  `{"type":"error","error":{"type":"authentication_error","message":
  "API key is invalid."},"request_id":null}`.

So the turn layer sees 11 turns, 11 error turns, 1 probe excluded.

Normalisation: `test/normalize.py --repo` (pids/tids → P/T ids,
seq/ts stripped, loopback port → `PORT`, repo path → `$REPO`) plus a
`/home/tester` → `$HOME` sweep on events; the same path/port
substitutions applied as strings to the `llm/` blobs. Blob bodies are
otherwise verbatim — each request body is Claude Code's ~100 KB system
prompt (the 11 copies are byte-identical, so git stores one).

Key-safety — re-run after any regeneration (last: 2026-10-03):

```
grep -r -i -E 'sk-[a-z]+-[A-Za-z0-9_-]{10,}|Bearer [A-Za-z0-9._-]{10,}' .   # 0
grep -r -o '"x-api-key":"[^"]*"' llm/*.req.json | sort | uniq -c            # REDACTED ×11
grep -r -c '/home/tester' .                                                 # 0
```

The proxy redacts `x-api-key`/`Authorization` before storing (no
Authorization header is stored at all) and the tracer records syscalls
only — TLS payloads travel encrypted.

Success-path counterpart is still missing: see issue #2
("record success-path real Claude Code fixture (needs API credits)").

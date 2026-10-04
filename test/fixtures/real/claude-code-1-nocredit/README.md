# claude-code-1-nocredit — REAL Claude Code no-credit capture (400, binary body)

A genuine `claude -p "list the files in this repo and count the lines of
C++"` session (request blob carries User-Agent `claude-cli/2.1.284`,
model `claude-opus-5-5`) recorded under `snowglobe run` against the
**real Anthropic API without credits**: the single real turn came back
HTTP 400 with a real 193-byte BINARY (non-JSON) body. The proxy stores
raw request/response bytes plus timing and never assumes JSON, so the
binary body is kept verbatim — this fixture is the format-agnosticism
proof. Salvaged 2026-10-04 from `~/claude-real.sgr` (the regenerated
capture from after the /tmp wipe lost the original error-path fixture;
that path is preserved separately as `claude-code-1-error/`).

Provenance: recorded 2026-10-02 17:55:25–17:55:28 UTC on WSL2 Ubuntu
24.04 as uid 1000 with the debug build (manifest: `snowglobe_version`
0.1.0-phase0, tracer ptrace, `event_count` 566).

Contents: **566 normalised events, 0 `trace.decode_error`**, 2
`llm.request` / `llm.response` pairs, 6 blobs (2 × req/res/idx; 0-byte
`*.tmp` transients dropped):

- id 0 — `HEAD /anthropic/api/hello` pre-flight probe (model-less,
  bodyless → NOT a turn; its response is the proxy's 502
  `{"error":"upstream failed"}`).
- id 1 — **1 real turn, status 400**, body 193 bytes of binary (not
  JSON: `JSON.parse` throws; `foldBody` stays empty in both modes and
  `errorInfo` stays null — never a guessed message). Request is
  `stream:true`, 1 chunk, `ttfb_ms` 612.

So the turn layer sees 1 turn, 1 error turn, 1 probe excluded — the
"every non-probe response is a turn, INCLUDING errors" rule (`model.ts`)
holds for non-JSON error bodies too.

Normalisation: `test/normalize.py --repo` (pids/tids → P/T ids,
seq/ts stripped, loopback port → `PORT`, repo path → `$REPO`) plus a
`/home/tester` → `$HOME` sweep on events; the same path/port
substitutions applied as strings to the `llm/` blobs (`0001.req.json`
only — the binary body and the probe blobs carry no paths). Blob bodies
are otherwise verbatim.

Key-safety — re-run after any regeneration (last: 2026-10-04):

```
grep -r -i -E 'sk-[a-z]+-[A-Za-z0-9_-]{10,}|Bearer [A-Za-z0-9._-]{10,}' .   # 0
grep -r -o -E 'x-api-key.{0,14}' llm | sort | uniq -c                       # REDACTED ×1 (+ 1 body-text mention of the redaction list)
grep -r -c '/home/tester' .                                                 # 0
```

The proxy redacts `x-api-key`/`Authorization` before storing (no
Authorization header is stored at all) and the tracer records syscalls
only — TLS payloads travel encrypted. The remaining `authorization` /
`x-api-key` hits are words inside the recorded prompt (our own AGENTS.md
§9, embedded in Claude's system reminder), never header values.

Success-path counterpart is still missing: see issue #2
("record success-path real Claude Code fixture (needs API credits)").

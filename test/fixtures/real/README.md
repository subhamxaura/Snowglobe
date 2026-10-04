# Realistic-workload fixtures (NOT goldens — never diff-compared)

`toy-agent-3turn/` — FALLBACK dev data for the 1C viewer (see its README):
toy agent, 3 LLM turns, normalised events + request/response blobs. Stands
in for a real Claude Code trace because no API key exists on the recording
box; the "record real-agent fixtures" issue stays open.

`pip-download-requests/events.jsonl` — normalised trace of
`python3 -m pip download requests -d <tmp>` under `snowglobe run`.
Stands in for a real-agent recording until API keys are available on the
recording box (see issue "record real-agent fixtures" — no keys were
present when this was recorded; a key-presence anomaly during the session
is documented in STATUS.md).

Provenance: recorded 2026-09-27 on WSL2 Ubuntu 24.04 as uid 1000 with the
debug build. 1171 normalised events, 0 `trace.decode_error`.

Key-safety: verified with `grep -c -i -e sk-ant -e x-api-key` → 0.
The tracer records syscalls only; TLS payloads (including any
Authorization header) travel encrypted and are never stored. Never commit
a fixture without re-running that check.

`claude-code-1-error/` — REAL Claude Code recording (the real-data
gate): `claude -p hi` against the real Anthropic API with an invalid
key. 735 normalised events, 0 decode_error, 1 probe + 11 turns all
HTTP 401 with real Anthropic error envelopes, 36 blobs. Full provenance
+ secret-audit recipe in its README.

`claude-code-1-nocredit/` — REAL Claude Code no-credit capture
(salvaged 2026-10-04 from `~/claude-real.sgr`, the regenerated capture
from after the /tmp wipe): 566 normalised events, 0 decode_error,
1 probe (502) + 1 real turn HTTP 400 with a 193-byte BINARY (non-JSON)
body stored verbatim — the proxy format-agnosticism proof, 6 blobs.
Full provenance + secret-audit recipe in its README.

Success-path real fixture is still missing — issue #2 stays open.

# Realistic-workload fixtures (NOT goldens — never diff-compared)

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

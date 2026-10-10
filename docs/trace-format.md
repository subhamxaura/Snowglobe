# Trace format — schema v0 (source of truth)

Status: Phase 1B implementation (schema still 0: all changes are additive
optional fields). Any change bumps `schema` in manifest.json and ships a
migration note here (AGENTS.md §3).

## Layout

```
<run>.sgr/
  manifest.json   schema, snowglobe_version, started/finished (RFC3339),
                  cmd[], cwd, project, kernel, tracer, isolate{},
                  env_fingerprint (sha256 of sorted env NAMES), event_count,
                  last_hash, file_hashes{}
  events.jsonl    one event per line, seq strictly increasing from 0
  llm/            per-request blobs, see "LLM blobs" below (Phase 1B+)
```

Phase 0 writes `isolate:{}`, `file_hashes:{}` (populated in Phase 2).
Isolate writes `isolate:{on:true,features:[userns,mount,pid,overlay],upper:"overlay/upper",masks[],allow_path[]}` (masks = masked `~/.ssh|~/.aws|~/.gnupg` minus `--allow-path`; `upper` is `overlay/upper`, `etc-upper`/`home-upper`/`fs-rw-N` are not for apply),
`finished:null` until the run completes. Partial traces (crash/kill) keep
`finished:null` and remain readable.

## links.json — derived causal sidecar (Phase 1D, ADR-0006)

`links.json` sits next to `events.jsonl` (`<run>.sgr/links.json`) and is
**derived, not evidence**: it is explicitly outside the event hash chain,
carries no hashes, and is regenerable at any time via
`snowglobe link <run>` (deterministic: same input → byte-identical
output). Its own `version` starts at 1; link-format changes never bump
trace `schema`.

Shape (all keys fixed order, compact):

```json
{"version":1,"turns":[{"turn":0,"llm":{"req":107,"res":108,"tools":["call_1"]},
"attributed":[{"seq":113,"basis":"argv-match","confidence":"high"}],
"unattributed":[{"seq":0,"reason":"pre-turn"}]}]}
```

- `turn` is the llm id (mirrors the viewer's turn id). `llm.req/res`
  are order keys (seq; file index for seq-less normalised fixtures) of
  the request/response events; `tools` are tool_call ids parsed from the
  response body (empty for error/binary bodies).
- `basis` is one of `window` (side effect inside the turn span),
  `lineage` (process born in the turn, acting later), `argv-match`
  (exec argv carries the turn's tool command — upgrades the other two).
  `confidence` is `high` for all three today (schema room for more).
- `unattributed` entries carry a `reason` (`pre-turn`: before the first
  response). Probes and `llm.*` skeleton events never appear in either
  list. Readers must ignore unknown basis/reason/confidence values.
- Traces with no turns yield `"turns":[]` (nothing is attributable;
  `snowglobe link` still exits 0 and says so).

## baseline.json (Phase 3, isolate runs)

`<run>/baseline.json`: `{"version":1,"project":"<abs>","excludes":[…],
"files":{"rel":{"type":"file","sha256":…,"size":N,"mtime":T} |
{"type":"link","target":…,"mtime":T}},"skipped_special":N}` (keys sorted;
default excludes `.git/`, `.snowglobe/`, `node_modules/` plus
`--baseline-exclude`; fifos/sockets/devices counted, never recorded;
unreadable files abort the run). Manifest points at it
(`"baseline":"baseline.json"`, isolate runs only; schema stays 0).
`mtime` is forensic; diff/apply key on sha256 (files) and targets (links).

## diff / apply / compare change sets (Phase 3)

Change sets are derived at command time (upper-vs-baseline, never
stored): `(scope, path)` with scope ∈ project/etc/home/fs-rw-N; kinds
A/M/D/R (R = whiteout + same-hash add, non-empty files only; symlinks
never pair) plus S (special files: shown, never applied). `diff` shows
the *pending* subset (host already equal to upper is suppressed as
applied, counted); hunks come from host bytes verified against baseline
hashes (mismatch → letter kept, hunks withheld, `host-changed` note).
`apply` validates all then writes (exit 65, nothing written, on
conflict/rejection); `compare` joins two raw (host-independent) sets.
Non-isolate runs: event-derived paths only (`created?` etc. plus the
content-absent line). Every content `diff` persists review artifacts
into the run dir: `fs/diff.patch` (the same bytes `--patch=FILE`
writes: informational, `apply` reads the upper, never the patch) and
`fs/summary.json` (`{"version":1,"changes":[{scope,path,kind,newPath?,
adds,dels,oldSha?,newSha?,linkTarget?,hostChanged?,noOldBytes?}],
"counts":{A,M,D,R,S},"applied":N,"opaque_unreadable":N}` — pending set
only, deterministic, no absolute paths).

## Common fields

`seq, ts_us (CLOCK_REALTIME), t_ms (monotonic ms since run start), ev, pid,
tid, prev_hash, hash` where `hash = sha256(prev_hash || line_without_hash)`.
Genesis `prev_hash` is `"0"`. `turn`/`tool_call` are null/absent until Phase 1D
causal linking.

`pid` is the thread-group id (tgid); `tid` is the kernel thread id. For
single-threaded processes they are equal. Thread creation is a
`proc.start` with `thread:true`; a thread's death is a `proc.exit` with its
own tid. Threads that vanish in an exec without an exit stop get
`proc.exit` with `vanished:true` (no `code`/`signal`).

## Events emitted in Phase 0

| ev | fields |
|---|---|
| `run.meta` | `cmd[], cwd, backend ("ptrace"\|"notify")` — plus `isolate:true, masks[]` under `--isolate` (masked secret paths, minus `--allow-path`) |
| `proc.start` | `ppid, root?, thread?` |
| `proc.exec` | `path, argv[], cwd` — plus `truncated:true` when argv was cut (64-entry cap) |
| `proc.exec_failed` | `path, errno` — ptrace only (see notify delta below) |
| `proc.exit` | `code, signal` — or `vanished:true` (exec-vaporised thread) |
| `fs.open` | `path, write, create, trunc, fd` — plus `tmpfile:true` for O_TMPFILE (path = directory) |
| `fs.unlink` | `path, ok` |
| `fs.rmdir` | `path, ok` (unlinkat with AT_REMOVEDIR) |
| `fs.rename` | `from, to, ok` |
| `fs.mkdir` | `path` |
| `fs.symlink` | `target, path, ok` |
| `fs.chmod` | `path, mode, ok` (mode = octal string, e.g. `"0755"`) |
| `net.connect` | `family (ipv4\|ipv6\|unix\|unspec\|unknown, else family=N), addr (legacy formatted string, kept), ip + port (number) for ipv4/ipv6, path for unix, ok, initiated` (initiated:false = refused/unreachable; true also on -EINPROGRESS) |
| `net.disconnect` | `ok` — a `connect()` whose sockaddr has family AF_UNSPEC (the UDP-disconnect idiom: the resolver unconnecting a datagram socket). No peer fields; there is no peer. |
| `net.sendto` | same endpoint fields as `net.connect` (minus `initiated`) (UDP/DNS visibility) |
| `net.bind` | same endpoint fields as `net.connect` (minus `initiated`) |
| `llm.request` | `id, provider (openai\|anthropic\|gemini\|custom\|unknown), method, path, model (string or null), bytes, stream` — `pid` is the *supervisor* (the proxy lives there), `tid` the handler thread; the stored envelope is `llm/NNNN.req.json` by id convention |
| `llm.response` | `id, status, bytes, ttfb_ms (first body byte; null when none), total_ms, chunk_count, truncated, req, res, idx` (last three are `llm/…` relative paths) |
| `trace.decode_error` | `syscall, errno, reason` |

## Notify-backend delta (`--backend=notify`, ADR-0009)

The notify backend observes syscall *entry* only, so outcome events
carry **`result_known:false`** (additive optional key, schema stays 0)
instead of outcome keys:

| ptrace event | notify shape |
|---|---|
| `fs.open{…, fd, [ok, errno]}` | `fs.open{path, write, create, trunc, [tmpfile], result_known:false}` — no `fd` (unknown at entry), no `ok`/`errno` |
| `fs.unlink/rmdir/rename/symlink/chmod{…, ok}` | same fields minus `ok`, plus `result_known:false` |
| `net.connect/sendto/bind/disconnect{…, ok[, initiated]}` | endpoint fields only, plus `result_known:false` |
| `proc.exec{…}` (success) | same fields, plus `result_known:false` (attempt semantics) |
| `proc.exec_failed{path, errno}` | **never appears** — a failed exec is indistinguishable at entry, so it surfaces as `proc.exec{result_known:false}` |
| `fs.mkdir{path}`, `proc.start/exit`, `run.meta`, `llm.*` | identical (no outcome keys in either backend) |

Consequences: default filters apply except the failure-based read-open
drop (failed probes to non-noisy paths appear as unknown attempts);
`--isolate` + notify is rejected (69); the viewer renders unknown as an
explicit badge, never as ok. x32-ABI numbers (high bit set) classify
identically to native in both backends (bit stripped before match;
pinned by the `x32` scenario). Parity definition (subsequence + shape
rules incl. the exec_failed→exec mapping) lives in
`test/parity/check_parity.py`; pinned streams in
`test/fixtures/scenarios/*/expected-notify.jsonl`.

`net.dns` (proxy-only mode), `fs.*` (other), `trace.dropped` arrive in
Phase 2+. `turn`/`tool_call` causal linking arrives in Phase 1D.

> Deviation from the AGENTS.md §3 sketch, recorded here deliberately:
> there are no `llm.chunk` events and no `blob/usage/cost_usd/tool_calls`
> fields. Per-chunk timing lives in `.res.idx` (chunk volume would explode
> event counts), and all provider parsing (tokens, cost, tool calls) lives
> in the TypeScript viewer per the locked format-agnostic decision. The
> sketch's shape may still guide Phase 3 replay hashing; any adoption bumps
> `schema` with a migration note.

## LLM blobs (`llm/`, Phase 1B)

`NNNN` = zero-padded request `id` (`%04d`).
- `NNNN.req.json`: `{"method","path","provider","headers":{…},"body","body_encoding"}`
  — headers stored redacted (`Authorization` → `REDACTED`; forwarding is a
  separate, byte-identical path). `body` is the verbatim request bytes as
  a UTF-8 string, or base64 with `"body_encoding":"base64"` when not valid
  UTF-8 (bodies are never token-redacted: replay needs them verbatim).
- `NNNN.res.json` / `.sse` / `.bin`: raw upstream response bytes, picked by
  response `Content-Type` (event-stream → `.sse`, JSON → `.json`, else
  `.bin`). Written to `.tmp` during the stream, renamed when complete.
- `NNNN.res.idx`: one `{"off":<bytes received so far>,"ts_us":<wall arrival>}`
  per received body chunk — the streaming-latency record. Empty for
  header-only outcomes (unknown-route 404, upstream-unreachable 502, which
  synthesize their own JSON bodies instead).
- `truncated:true` means the downstream went away mid-stream (abort) or the
  upstream broke mid-stream after a complete head — the stored prefix is
  partial. A clean empty body (204, empty error page) is *not* truncated.
- The run epilogue's "N LLM turns" counts 2xx responses only.

## Replay sidecars (Phase 5, ADR-0010 — schema stays 0)

Replay re-serves the blobs above; response *headers* were never stored,
so a replayed response carries status (from its `llm.response` event)
+ Content-Type (from the blob extension) + the byte-exact body, sliced
at the recorded `.idx` `off` bounds. `off` is bytes received *before*
that chunk; an empty `.idx` serves the whole body as one chunk. HEAD
requests re-serve status-faithfully with an empty wire body (HTTP
suppresses HEAD bodies; the stored bytes and the event's `bytes` keep
the recorded values for forensics).

- Manifest gains `"replay_of":"<abs original run>"` on replay runs
  only (additive; live-run manifests are byte-identical to before).
- `llm.response` gains `"replay_of":<recorded turn id>` on replayed
  responses only (the compare join key; live runs never carry it).
- `<new-run>/replay-report.json` (`version:1`): `original`, `replay`,
  `original_exit`/`replay_exit` (the agent exit is *compared*, and the
  same nonzero exit is clean), `turns:{original,replay,match}`,
  `order_matches`, `unrecorded` (loud-502 count), `ignores[]`, and
  `categories:{llm,fs,proc,net,exit}` each
  `{status:identical|diverged|unrecorded, detail, replay_only[],
  original_only[]}` (`unrecorded` only occurs for `llm`; evidence
  capped at 20 lines per side). The `llm` section additionally carries
  `served_exact` (turns served by primary hash match) and
  `served_fallback` (turns served by the order-preserving endpoint
  fallback); exact + fallback + `unrecorded` always equals the replay
  turn count. Readers must ignore unknown categories.
- `<orig-run>/replay.unrecorded.jsonl`: one
  `{"ts_us","method","path","provider","body_sha256","body_bytes",
  "reason":"no-recorded-match"}` per MISS (the original run is never
  otherwise modified by replay).
- Volatility excluded from the compare (documented, never silent):
  `seq/ts_us/t_ms/t_us/prev_hash/hash/fd/backend`, pid→Pn/tid→Tn,
  ports→PORT, `/tmp/tmp.*`→`$TMP` (incl. mkdtemp `_` suffixes),
  `127.0.0.1:N`/`[::1]:N`→PORT, python-version folds (the
  `test/normalize.py` rules), request keys
  `id/tool_call_id/tool_use_id/created/timestamp/ts/user/request_id/
  session_id`, and timing fields `ttfb_ms/total_ms` + the join key
  `replay_of`. `--ignore FIELD` strips extra event fields.

## Filtering (default; `-a`/`--all-opens` disables)

- Read-only opens (`write=false`) that fail, or target noisy paths, are skipped.
- `O_DIRECTORY` / `O_PATH` opens are skipped (handles carry no content).
- `O_TMPFILE` opens are recorded as `write:true` with `tmpfile:true`.
- Noisy prefixes: `/proc/`, `/sys/`, `/dev/`, `/etc/ld.so*`, `/etc/passwd`,
  `/etc/nsswitch*`, `/usr/share/locale`, `/usr/lib/locale`.

## Path rules

Absolute + lexically canonical (`..`/`.`/double-slash resolved). Relative paths
resolved against `/proc/PID/cwd`; `dirfd ≠ AT_FDCWD` resolved against
`/proc/PID/fd/N`. Non-Linux readers must ignore unknown fields.

## Example lines

```json
{"ts_us":1727347200123456,"t_ms":12,"ev":"proc.exec","pid":1234,"tid":1234,"path":"/usr/bin/sh","argv":["sh","-c","echo x"],"cwd":"/tmp","seq":1,"prev_hash":"abc…","hash":"def…"}
{"ts_us":1727347200123789,"t_ms":13,"ev":"fs.open","pid":1234,"tid":1234,"path":"/tmp/f","write":true,"create":true,"trunc":true,"fd":3,"seq":2,"prev_hash":"def…","hash":"123…"}
```

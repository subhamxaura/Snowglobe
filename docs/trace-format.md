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
content-absent line). The run dir does not (yet) carry
`fs/diff.patch`: `diff --patch=FILE` writes a user-chosen file.

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
| `run.meta` | `cmd[], cwd` — plus `isolate:true, masks[]` under `--isolate` (masked secret paths, minus `--allow-path`) |
| `proc.start` | `ppid, root?, thread?` |
| `proc.exec` | `path, argv[], cwd` — plus `truncated:true` when argv was cut (64-entry cap) |
| `proc.exec_failed` | `path, errno` |
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

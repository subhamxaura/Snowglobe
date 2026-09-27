# Trace format — schema v0 (source of truth)

Status: Phase 1A Block 1 implementation (schema still 0: all changes are
additive optional fields). Any change bumps `schema` in manifest.json and
ships a migration note here (AGENTS.md §3).

## Layout

```
<run>.sgr/
  manifest.json   schema, snowglobe_version, started/finished (RFC3339),
                  cmd[], cwd, project, kernel, tracer, isolate{},
                  env_fingerprint (sha256 of sorted env NAMES), event_count,
                  last_hash, file_hashes{}
  events.jsonl    one event per line, seq strictly increasing from 0
```

Phase 0 writes `isolate:{}`, `file_hashes:{}` (populated in Phase 2),
`finished:null` until the run completes. Partial traces (crash/kill) keep
`finished:null` and remain readable.

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
| `run.meta` | `cmd[], cwd` |
| `proc.start` | `ppid, root?, thread?` |
| `proc.exec` | `path, argv[], cwd` |
| `proc.exec_failed` | `path, errno` |
| `proc.exit` | `code, signal` — or `vanished:true` (exec-vaporised thread) |
| `fs.open` | `path, write, create, trunc, fd` — plus `tmpfile:true` for O_TMPFILE (path = directory) |
| `fs.unlink` | `path, ok` |
| `fs.rmdir` | `path, ok` (unlinkat with AT_REMOVEDIR) |
| `fs.rename` | `from, to, ok` |
| `fs.mkdir` | `path` |
| `fs.symlink` | `target, path, ok` |
| `fs.chmod` | `path, mode, ok` (mode = octal string, e.g. `"0755"`) |
| `net.connect` | `addr (formatted), ok` |
| `net.sendto` | `addr, ok` (UDP/DNS visibility) |
| `net.bind` | `addr, ok` |
| `trace.decode_error` | `syscall, errno, reason` |

`llm.*`, `net.dns`, `fs.*` (other), `trace.dropped` arrive in Phases 1B/2.

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

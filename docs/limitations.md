# Known limitations (tracer + proxy capture + `--isolate` visibility)
Snowglobe is *isolation and visibility*, never a security boundary for hostile
code (see `docs/threat-model.md` in Phase 2). This page lists what the
tracer backends cannot see or cannot do, with the mechanism in each case.

## Notify-backend semantic delta (entry-only observation)

`--backend=notify` (ADR-0009) traps syscalls at entry via
SECCOMP_RET_USER_NOTIF; there are no syscall-exit stops, so outcomes are
unknown. Outcome events carry `result_known:false` instead of
`ok`/`errno`/`fd`; `proc.exec_failed` never appears (a failed exec
surfaces as `proc.exec` with `result_known:false`); `run.meta` carries
`backend:notify`. The default filters apply except the failure-based
read-open drop — failed probes to non-noisy paths are recorded as
unknown attempts. The viewer renders unknown as an explicit badge,
never as ok. Full table: `docs/trace-format.md` § "Notify-backend
delta"; parity relation: `test/parity/check_parity.py`.

What notify does NOT change: TOCTOU in path resolution (below) is
identical — entry-time reads race the same way exit-time reads do.
`--isolate` + notify is rejected (69): the isolate middle needs its own
listener hand-over first (issue #6).

## Non-native-arch visibility (both backends, explicit rule under notify)

Compat-arch syscalls (e.g. i386-on-x86_64, different audit arch AND
different numbers) are lifecycle-only under both backends — only
proc.start/exec/exit is recorded for such processes:

- ptrace does it implicitly: the decoder keys off native `SYS_*`
  numbers and never reads the syscall-info `arch` field, so
  compat-arch numbers match nothing (`Kind::None`) and are ignored
  silently. Lifecycle still tracked.
- notify does it explicitly: the BPF filter checks the audit arch
  first, and a mismatch falls through to `SECCOMP_RET_ALLOW` — the
  syscalls run untrapped (zero stops) with lifecycle-only tracing.
  This is a deliberate visibility degrade, not a sandbox deny (the
  isolate filter kills instead — different tool, different rule;
  `test_notify.cpp` pins ALLOW-on-mismatch).

In both cases a compat-arch child keeps running with full lifecycle
tracing; its file/network syscalls are invisible. Rebuild the tracee
for the native arch if you need its syscalls observed — neither
backend can do it.

## x32 numbers are observed identically in both backends

Same-arch x32 syscalls (native audit arch, bit 30 set in nr) are NOT a
visibility gap: both backends strip the bit before classifying
(shared `stripX32Nr`, identity for native nrs), so an x32 openat
decodes exactly like a native openat — same event kinds, same paths,
same filters, with only the documented entry/exit delta between them
(`result_known:false` under notify). The notify BPF traps the high
range (`JGE`, pinned by the `test_notify.cpp` emulator) so x32 stays
observed, never allowed unseen; the `x32` golden scenario pins one
x32 mkdirat end to end in both streams (outcome-free by design — see
`sg_x32.c` for why openat would flake across kernels with and without
a strict x32 ABI).

## Causal attribution anchoring (links.json vs viewer heuristic)

Two attribution rules coexist by design and differ in anchoring:

- `links.json` (`core/link/`, ADR-0006) is **response-partitioned
  (res-partition, R1)**: turn N owns side effects in `[response N,
  response N+1)` in res-completion order (plus lineage retention for
  background children). Span lookup uses a res-sorted index, so
  concurrent out-of-order completions attribute to the last completed
  response. An event after request N+1 but before response N+1 still
  belongs to turn N — an in-flight request moves no boundary. Anything
  before the first response is explicitly `unattributed` (`pre-turn`),
  never guessed.
- The viewer fallback (`TurnIndex` without a sidecar) is
  **request-anchored**: `[request N, request N+1)`. Where links.json
  speaks it wins; unlisted seqs keep the old span answer. The UI labels
  which source is active (`linkage: sidecar` vs `linkage: heuristic`).

What attribution cannot know either way: in-memory-only effects (no
syscall, no event), which async worker inside one process acted (threads
share the pid, so lineage is process-grained), and anything the tracer
never saw (e.g. the io_uring blind spot above — absent events attribute
to nothing).

## Setuid binaries lose privileges under ptrace

A ptraced process cannot gain privileges via setuid/setgid exec (the kernel
strips the elevation; the process also becomes non-dumpable). Inside a traced
tree, `sudo`, `su`, `ping` (where setuid), `passwd` used interactively, and
similar tools run **unprivileged and may fail** (e.g. `sudo` refuses,
`passwd` cannot lock `/etc/shadow`). The trace still records an ordinary
`proc.exec` plus whatever the tool managed to do — see the `failing_exec`
scenario (`/usr/bin/passwd --help`, which needs no privilege). Agents that
must test privilege escalation cannot do it under Snowglobe.

## One tracer per process

The kernel allows a single tracer per thread. Anything inside the tree that
tries to ptrace — `strace -f`, `gdb --attach`, ASan/LSan/TSan runtimes that
inspect the process, Node's `--inspect` debugger agent — fails, usually with
`EPERM`. Concretely: never run an ASan/LSan/TSan-*instrumented binary as a
*tracee* (LeakSanitizer aborts outright: "does not work under ptrace"); our
own scenario helpers build with `-fno-sanitize` for this reason. Sanitizing
the *observer* (`snowglobe` itself) is fine and covered by the asan-ubsan CI
legs. Likewise, never nest `snowglobe run` inside `snowglobe run`.

## io_uring is a blind spot

File and network I/O submitted via `io_uring` (registered buffers, fixed
files, multishot accept) never appears as syscalls at submission time, so the
tracer cannot see it. `snowglobe run` sets `UV_USE_IO_URING=0` in the
tracee environment (without overriding an explicit user setting), which
forces libuv-based runtimes (Node ≥ 18 file ops) back onto the syscall path.
That covers Node only: any agent or tool submitting io_uring directly
(`io_uring_enter` is not decoded) stays invisible. Interpret the absence of
file events from such tools as "not observed", never as "no I/O happened".

## TOCTOU in /proc path resolution

Paths are resolved by reading `/proc/PID/cwd` and `/proc/PID/fd/N` *after*
the syscall stop. A multithreaded tracee can `chdir`, close, or rename
between the syscall and our read, so the recorded path can occasionally name
the wrong object. All resolution is lexical after the read (no `openat2`
`RESOLVE_IN_ROOT` semantics). Treat paths as best-effort attribution, not as
an audit log.

## ptrace overhead class

Every decoded syscall costs several context switches plus argument reads via
`process_vm_readv`. Measured Phase 1A baselines (`bench/results/`) on the
order of 3–14× wall clock depending on syscall density; the seccomp-notify
backend (Phase 4) exists to bring this under 1.15× on `npm install`/`pytest`.
Never use `--tracer=ptrace` timings for performance claims about the agent.

## Interpreters' startup noise vs the default filters
CPython/Node startups open dozens of libraries, locale data, and (without
`-B -S`) site files whose exact sets differ per distro release — unfixable
by normalization (import closures and link order differ, not just paths).
Test fixtures therefore avoid interpreters (compiled C helpers, coreutils,
dash); the default filters (`-a`/`--all-opens` disables) drop failed
read-opens, noisy system paths, and `O_DIRECTORY`/`O_PATH` handles. What
remains in a trace is behavior, not loader chatter — but a golden recorded
on one distro still pins that distro's loader file set; treat loader opens
as environment, agent file access as signal.

## Proxy capture: base-URL injection only (no MITM)

LLM capture works by pointing SDKs at the local proxy through base-URL
environment variables. Anything that does not honor them is not captured
as LLM traffic (it is still visible as `net.connect` from the tracer):

- **Agents/SDKs that ignore base-URL env vars** (hardcoded endpoints,
  in-process model calls, sidecar daemons): invisible as `llm.*`. If the
  agent you need falls here, file an issue with the SDK name — the fix is
  per-SDK plumbing or `--mitm`, not a proxy change.
- **HTTPS_PROXY-only SDKs**: proxy-tunnel env vars route *through* a proxy
  without rewriting the authority; our proxy is a base-URL *target*, not a
  CONNECT tunnel. Same file-an-issue rule.
- **HTTP/2**: cpp-httplib speaks HTTP/1.1 only. Clients that insist on h2
  prior-knowledge fail; clients that negotiate (ALPN/h2c upgrade) fall back
  to HTTP/1.1 and work. Upstream is always HTTP/1.1.
- **`--mitm` deferred**: TLS interception (custom CA + per-SNI forging) is
  a CLI flag today that is not implemented. No silent fallback exists: if
  traffic does not reach the proxy, there are no `llm.*` events, full stop.
- **Uninjected providers**: `AZURE_OPENAI_ENDPOINT` / `OLLAMA_HOST` from
  the CLI contract are not injected yet, so Azure/Ollama traffic is not
  captured. (Anthropic/OpenAI/Gemini are.)
- **Uploads are buffered**: request bodies accumulate in memory per request
  (50 MB verified; a hostile gigabyte body would transiently allocate a
  gigabyte in the supervisor). Downloads stream with zero application
  buffering.

## WSL interop + IPv6-failure patterns (from a real Claude Code run)

Observed in a real Claude Code run on WSL (error-path fixture lost to a
`/tmp` wipe before it could be committed — see STATUS.md; it will be
regenerated via an invalid-key run producing a real 401 through the proxy
and committed as `test/fixtures/real/claude-code-1-error/`), recorded here
so future traces with the same shapes are read correctly:

- **`/run/WSL/<pid>_interop` unix connects (ok:true) are the WSL interop
  channel** — the tracee spawning Windows-side processes (`claude.exe`
  helpers, `node.exe`, `powershell.exe`). They are normal on WSL, carry no
  agent payload, and must not be mistaken for exfiltration: the peer is the
  local WSL service, not the network. Same for `unix:/var/run/nscd/socket`
  (the resolver cache) and tool-private sockets such as
  `unix:/run/user/1000/cc-socks/*.sock` (Claude Code's own sandbox
  channel — interesting as behavior, opaque as bytes).
- **IPv6 attempts failing on IPv4-only/NAT64 networks look like errors but
  are normal**: happy-eyeballs racing emits `net.connect` to `[ipv6]:443`
  (or `:0` probes) with `ok:false`, immediately followed by a working IPv4
  connection. Read a lone failed IPv6 connect as connection-racing noise,
  not as an outage — only a run with *no* successful follow-up is
  suspicious. (The `AF_UNSPEC` disconnects nearby are the resolver
  unconnecting UDP sockets after use — recorded as `net.disconnect`.)

## Empty /proc under --isolate (no fresh procfs on this kernel)

New proc/sysfs instances inside the isolate user namespace are denied
(EPERM — see issue #4, probe/proc_pidns.c), so the
merged `/proc` is an empty tmpfs, not a fresh procfs. Host pids stay
invisible either way, but **runtimes needing `/proc/self` content abort
at startup**: Claude Code (Bun v1.4.3) dies SIGABRT after its
`/proc/self/maps|cgroup|statm|stat` opens fail (0 LLM turns — the
Phase-2 real-runtime gate is blocked on this, not on EROFS). Python
agents run fine. The gate re-runs when kernels allow fresh procfs.

## diff/apply/compare limits (Phase 3)

- **Agent `rmdir` of an emptied merged dir can fail EIO.** Observed on
  6.6.87-WSL2: deleting every file inside an overlay-merged directory
  works (per-file whiteouts), but the final `rmdir` of the emptied dir
  returns EIO, so the dir itself survives in the upper. Per-file deletes
  diff/apply cleanly; directory whiteouts (when the kernel produces
  them) delete recursively. The golden covers dir deletes with
  hand-made whiteouts instead of agent `rm -rf`.


- **Rename detection is content-hash exact.** A rename whose bytes changed
  in transit reports as A+D (never a guessed R); empty files never pair.
  Directory renames surface as per-file A/D pairs (dirs are structural,
  not changes).
- **Non-isolate runs have no content.** `diff` there lists event-derived
  paths (`created?`/`modified?`/`deleted?`/`renamed?`) with an explicit
  content-absent line — kinds are last-event guesses, never hunks.
- **Exclude-list heuristics.** Default excludes (`.git/`, `.snowglobe/`,
  `node_modules/`, plus `--baseline-exclude`) hide those subtrees from
  baseline, diff, and apply alike; agent behavior there is invisible by
  policy. `.git` is the exception in one direction only: always walked
  for review (tamper must be visible), never written by `apply`. Build
  outputs (`build/`, `target/`, `dist/`) are NOT excluded by default —
  baseline cost is linear in bytes hashed (this repo: 68.6s unfiltered at
  15,865 files incl. 1.6 GB `build/`, 17.1s with
  `--baseline-exclude=build`; debug binary, single-threaded portable
  SHA-256), so exclude them explicitly on big repos.
- **Untracked metadata.** Empty directories, mode-only changes, mtimes,
  and ownership are not changes (`diff` skips them; `apply` does not
  restore mtimes). `apply` preserves the upper file's mode bits.
- **Opaque-dir xattr may be unreadable** (EPERM on `trusted.overlay.*`):
  counted in the summary; explicit whiteouts still classify D.
- **`diff --patch` is informational.** `apply` reads the upper, never the
  patch; patch output is for review and bisection, not replay.

## Secret masks + ssh remotes under --isolate

`~/.ssh`, `~/.aws`, `~/.gnupg` are empty-tmpfs masked by default (0700);
`run.meta.masks[]` + manifest `isolate.masks[]` record the masked paths.
`--allow-path PATH` (repeatable, absolute, exact-or-parent) exempts —
ssh-based git remotes need `--allow-path ~/.ssh` (else `git fetch` over ssh
sees an empty `.ssh` and fails auth, by design).

## prlimit fallback (no cgroup delegation)
When cgroup v2 delegation is denied, the child still gets `RLIMIT_NPROC=512`
(`ulimit -u`), `RLIMIT_NOFILE=1024` (`ulimit -n`) always, and `RLIMIT_AS`
only when `--memory-max` was explicit (`ulimit -v` finite; unlimited
otherwise — AS limits break Bun/Node). `doctor --isolate` shows
`cgroup: no (prlimit fallback active)` and the run log notes it.

## Replay honest wall (Phase 5, ADR-0010)

Replay is honest re-execution, never claimed bit-determinism. Read this
before trusting a green replay:

- **External non-LLM traffic is NOT stubbed.** There is no netns yet,
  so the agent's direct network calls re-execute for real against
  whatever listens now. The toy agent's `http_get` fetches the URL the
  *recorded* model wrote into the tool args — replaying against a
  different server (or none) fails or diverges by design. Keep recorded
  sidecar services running on their recorded ports for clean replays;
  the mutation suite pins the diverged case on purpose.
- **Wall-clock and randomness in agent code re-execute for real.**
  `--freeze-time` does not exist; timestamps, backoff sleeps, and
  `random()` run live. Agents that branch on time will diverge (and say
  so in the report).
- **Replay validates observable behavior, not bit-identity.** The
  compare is order-insensitive multisets (fs paths, exec argv sets, net
  endpoints) plus LLM turn parity. A request-order swap of two turns
  with *distinct* bodies is flagged via serve order; a swap of two
  turns with *identical* canonical bodies is interchangeable by
  construction and passes. Volatile request content (timestamps, nonces)
  defeats the hash and drops every turn onto the order-preserving
  endpoint fallback — concurrently-issued identical requests may then
  serve swapped, and the compare catches it for distinct turns via serve
  order. Each recorded response serves exactly once: identical retries
  consume successive entries (pinned by unit), so a retry storm drains
  into loud 502s rather than looping one response. Non-isolate fs
  compare is path-level: a same-path different-bytes rewrite is
  invisible without both uppers.
- **Replayed response headers are content-type-only.** Response headers
  were never stored; replay serves status + Content-Type (from the blob
  extension) + byte-exact body. Agents that branch on upstream headers
  (rate-limit, request-id) see defaults, not history.
- **Replays run unisolated.** `--isolate` replay is rejected; isolate
  originals compare on event-observable behavior only (upper content
  parity needs a future isolate-aware replay).
- **Recorded secrets stay redacted.** `run.meta` cmd may carry
  `REDACTED` tokens (ADR-0003); such a cmd cannot replay against a live
  provider — pass a `--` override instead.
- **`--policy/--fast/--freeze-time` do not exist.** Divergence policies
  (`fuzzy`/`live`), fast-forward, and frozen time are future work; the
  CLI rejects the flags loudly rather than half-honoring them.

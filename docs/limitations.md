# Known limitations (tracer + proxy capture + `--isolate` visibility)
Snowglobe is *isolation and visibility*, never a security boundary for hostile
code (see `docs/threat-model.md` in Phase 2). This page lists what the
ptrace backend cannot see or cannot do, with the mechanism in each case.

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

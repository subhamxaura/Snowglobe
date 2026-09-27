# Known limitations (tracer + `--isolate` visibility)

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

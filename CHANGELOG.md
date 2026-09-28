# Changelog

All notable changes to this project will be documented in this file.
Format: Keep a Changelog. Versioning: SemVer (schema v0 until v0.1.0).

## [Unreleased]

### Added (Phase 1B Block 1)
- LLM recording proxy (`core/proxy/`): HTTP/1.1 on 127.0.0.1:ephemeral,
  routes /openai|/anthropic|/gemini + /u/<base64url> + --upstream overrides,
  zero-buffering response streaming, raw blob + per-chunk .idx store,
  llm.request/response events (no per-chunk events).
- Secrets redaction (`core/redact/` + ADR-0003): 7 header names, URL
  userinfo, key shapes, query params, sensitive env values; applied to
  stored headers, proc.exec argv and run.meta cmd. Forwarded bytes untouched.
- Supervisor: proxy lifecycle, 6 base-URL env vars, --no-llm-proxy,
  ≤10 s drain, epilogue counts LLM turns.

### Fixed (Phase 1B Block 1 CI, run 36332355926)
- `cross-aarch64`: host x86_64 OpenSSL cannot be used by
  `aarch64-linux-gnu-g++` (`openssl/opensslconf.h` is arch-specific, then
  host libs mismatch). Added `SNOWGLOBE_WITH_SSL` (default ON); the cross
  job passes `-DSNOWGLOBE_WITH_SSL=OFF` (httplib without
  `CPPHTTPLIB_OPENSSL_SUPPORT`, no `OpenSSL::` link). Native jobs keep TLS;
  the cross job stays build-only. Verified: `aarch64` configure + build
  clean, `file core/snowglobe` = `ELF 64-bit LSB pie executable, ARM
  aarch64`.
- `golden_threads` (22.04 asan-ubsan only): ack-pipe exit handoff was racy —
  successor wake/exit is concurrent with predecessor exit, so `waitpid`
  order varied with observer speed (ASan slowdown flipped it).
  `sg_threads.c` now uses main-gated exits (main joins t3, then releases
  t2..t0 in turn): deaths are strictly t3..t0 in real time. Golden
  unchanged (same 28 events). Verified: debug 26/26, asan-ubsan 26/26 with
  20× `golden_threads` green, zero sanitizer findings.

### Added
- Phase 0 bootstrap: CMake+Ninja build, presets (debug/release/asan-ubsan/tsan),
  clang-format/tidy configs, Apache-2.0 LICENSE.
- `ITracer` interface + Linux ptrace backend (exec/open/unlink/rename/mkdir/
  connect/sendto/bind, ppid tree, fork-race pending set, AT_FDCWD/dirfd path
  canonicalisation, default noisy-path filters + `-a/--all-opens`, decode_error).
- `JsonlWriter` with per-event SHA-256 hash chain + manifest.json lifecycle
  (finished=null until exit; event_count/last_hash finalised).
- CLI: `run` (propagates child exit; 64/69/70 for own failures), `doctor`
  capability table, `ls`, `rm`, `version` (+ `--json` where specified).
- Tests: Catch2 unit (JSON escaper, SHA-256 vectors, hash-chain recompute),
  Python integration (open→rename→unlink golden + chain/manifest integrity,
  skips with reason off-Linux).
- Probes: userns, overlayfs_userns, landlock, seccomp_notif, cgroupv2, ptrace_scope.
- CI: ubuntu-22.04/24.04 × debug/asan-ubsan, clang-format check, aarch64 cross-build.
- Docs: docs/PLAN.md, docs/architecture.md, docs/trace-format.md (schema v0),
  docs/adr/0001-ptrace-first-seccomp-later.md, README (pre-alpha banner), STATUS.md.

### Fixed (found by executing on Linux — WSL2 Ubuntu 24.04, kernel 6.6)
- `PTRACE_SYSCALL_INFO_*` constants were 0/1/2; UAPI enum is NONE=0, ENTRY=1,
  EXIT=2, SECCOMP=3 — the tracer decoded zero syscalls before this fix.
- Successful `execve` now emits `proc.exec`: path/argv are read at ENTRY time
  (the old image is unmapped by EXIT; exit-time reads faulted with EFAULT).
- `JsonlWriter` emitted a trailing `}}` per line (invalid JSONL); chain
  recompute test added a single-object tripwire.
- `extern char** environ` moved to global scope (was namespace-scoped,
  undefined reference at link).
- `snowglobe doctor` no longer guesses: Landlock ABI via
  `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`,
  seccomp-notif via `seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &USER_NOTIF)`,
  overlayfs via a real unshare+self-map+mount dance in a forked tester
  (drops cleanly to nobody: empty groups + restored dumpability, else even
  self map files EACCES); AppArmor userns knob reported in the userns row.
- clang-format clean (`AllowShortCaseLabelsOnASingleLine: true` added);
  CI test step simplified to `ctest --preset ${{ matrix.preset }}`.
- CI: Ubuntu 22.04 no longer uses `pip install --break-system-packages`
  (22.04 ships pip 22.0.2, which predates the flag); 22.04 gets Python via
  setup-python and the Kitware cmake wheel through its modern pip.

### Added (Phase 1A Block 3)
- `tcp_loopback` scenario (C helper: 127.0.0.1:0 + ::1:0 binds, blocking
  connects, non-blocking connect asserting EINPROGRESS, closed-port connect
  asserting ECONNREFUSED); `net.connect` carries `initiated` (true on
  success/EINPROGRESS, false on refused/unreachable).
- `bench/ptrace_baseline.sh` (fork/exec ×300, python import, git status,
  find /usr/lib; 5 runs, medians, traced vs untraced) + local
  `bench/results/ptrace-baseline.md` + CI `bench` workflow committing
  `ptrace-baseline-ci-24.04.md` separately.
- `docs/limitations.md` (setuid, one-tracer, io_uring, ASan tracees, TOCTOU,
  overhead, interpreter noise); trace-format + AGENTS §3 synced.
- Realistic fixture `test/fixtures/real/pip-download-requests/` (1171
  normalised events, 0 decode errors; no API keys on the box, so the
  real-agent recording is a human-filed issue).

### Fixed (Phase 1A Block 3, D1 adversarial review of core/tracer/ptrace)
- Silent argv truncation now emits `truncated:true` (64-entry cap).
- Tracee death before first stop: external SIGKILL finalises normally;
  TRACEME failure (nested tracer) errors loudly instead of faking agent-127.
- Dead code removed (`evSeqHint`, `<set>`); signal coalescing fixed (atomic
  counter); errno captured before envelope build; oversized sockaddr emits
  decode_error; CLI warns once on trace-write failure; stale chmod comment
  corrected; supervisor failures return loud EX_SOFTWARE, never fake codes.

### Added (Phase 1A Block 2)
- Golden fixture suite: `test/fixtures/scenarios/` (fork_storm, threads,
  exec_chain, deep_dirs, unix_sockets, failing_exec) with `run.sh` +
  `expected.jsonl`; `test/normalize.py` (P/T ids, $TMP/$REPO, python-version
  paths) and `test/golden.py` (`--update` regeneration + unified diffs).
  threads/unix_sockets/exec_chain avoid interpreters (CPython startup file
  sets differ per release and cannot be normalised): compiled C helpers and
  pure sh+env instead — goldens regenerated accordingly.
- Automated kill tests: supervisor SIGKILL (no survivors in 2 s), root
  SIGKILL (exit 137 + finalised manifest), supervisor SIGTERM (exit 143).
- `probe/openat2.c`; openat2 flags now read from `struct open_how`.
- `fs.chmod` mode is an octal string (`"0755"`).
- Scenario C helpers build without sanitizers in every preset: an
  ASan-instrumented tracee loads libasan (golden divergence) and
  LeakSanitizer fails fatally under ptrace. `$HELPERS` normalises before
  `$REPO` so one golden serves all build presets.

### Added (Phase 1A Block 1)
- Threads: every event carries `pid` (=tgid) + `tid`; `proc.start` sets
  `thread:true` for clone-with-thread; `probe/threads.c` proves per-tid
  attribution (4 threads, 4 files, 4 distinct tids).
- Exec in a multithreaded process: vanished tids get `proc.exit` with
  `vanished:true`; exec details scavenged from the vanishing sibling so the
  single `proc.exec` survives even non-leader exec (`probe/mt_exec.c`).
- `fs.rmdir` (unlinkat + AT_REMOVEDIR), `fs.symlink` (symlink/symlinkat),
  `fs.chmod` (chmod/fchmod/fchmodat, mode as JSON number).
- Default filters skip `O_DIRECTORY`/`O_PATH`; `O_TMPFILE` records
  `write:true` + `tmpfile:true`. `UV_USE_IO_URING=0` forced in the child env.

### Fixed (Phase 1A Block 1)
- `PTRACE_EVENT_VFORK_DONE` resumes bare (was injecting stray SIGTRAP).
- ESRCH on GETEVENTMSG/GET_SYSCALL_INFO no longer emits bogus decode errors.
- Signals: first SIGINT/SIGTERM SIGTERMs the root only and keeps tracing to
  drain (exit code + finalised manifest); second SIGKILLs the tree.

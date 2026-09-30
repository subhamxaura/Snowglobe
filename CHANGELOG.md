# Changelog

All notable changes to this project will be documented in this file.
Format: Keep a Changelog. Versioning: SemVer (schema v0 until v0.1.0).

## [Unreleased]

### Fixed (real Claude Code run, `/tmp/claude-real.sgr`: 2 error turns, 0-byte tmps)
- Epilogue counted only 2xx as turns, so two error responses (502
  synthesized for a `HEAD` health check, 400 with 198 binary bytes)
  reported "0 LLM turns". Every recorded response is now a turn;
  `errors()` counts the non-2xx subset: `2 LLM turns (2 errors)`, `1 LLM
  turn (1 error)`, bare `N LLM turns` when clean. `--json` gains additive
  `"errors"`. Mock-402 test pins it (status + byte-exact blob).
- Leftover `0000.res.tmp` + `0000.res.idx.tmp`: the 502 headError branch
  opened `.tmps` then wrote finals directly. It now removes them, and the
  shutdown-detach path marks the flight so the worker removes its own
  (unlink-only from the handler side — closing another thread's streams
  would race). New no-tmp test covers 429 + disconnect + unreachable-502
  in one run and asserts zero `*.tmp` with all finals present.
- `connect()` with an AF_UNSPEC sockaddr (the UDP-disconnect idiom, seen 3×
  as `"addr": "family=0"`) is now `net.disconnect{ok}`, not a connect to
  nothing. `tcp_loopback` grew a UDP connect/disconnect pair to pin it.
- `net.connect`/`sendto`/`bind` carry structured endpoint fields —
  `family` (ipv4|ipv6|unix|unspec|unknown), `ip` + numeric `port` for IP,
  `path` for unix — with the legacy `addr` string kept verbatim.
  Goldens regenerated (tcp_loopback, unix_sockets); numeric ports
  normalise to `"PORT"`.
- `proxy_concurrent` is now pairwise: 8 distinct stream bodies, asserting
  stored<->sent and received<->stored bijections (identical bodies could
  not catch cross-stream contamination).

### Added (Phase 1C — embedded viewer)
- `viewer/` (Next.js 14 Pages-Router static export, system fonts, no CDN):
  turns (OpenAI/Anthropic full + SSE-delta folding, tool calls, static
  cost table), timeline, processes, files, network, search; 50 MB traces
  stream-parse with a virtualized list.
- `snowglobe view <run> [--port=7777] [--open]`: serves the embedded app
  + `/trace/*` from the run dir on 127.0.0.1 (ephemeral fallback with a
  note, traversal guard, `--open` via best-effort xdg-open). Embedding is
  pure CMake (`cmake/embed_viewer.cmake`, build-time, `SNOWGLOBE_VIEWER=OFF`
  escapes it); Node 18+ required otherwise.
- Tests: Vitest parser units (SSE deltas + committed toy-agent blobs),
  Playwright e2e (fixture renders 3 turns with the turn-2 file-write
  linkage, zero non-localhost requests asserted, 50 MB open < 3 s).
  New `viewer` CI job (vitest + chromium e2e); matrix legs run
  `viewer_unit`, e2e SKIP-prints without browsers.
- Measured: 80 KB first-load JS; 50 MB open ~1 s; toy-agent e2e green.
  Local: debug 36/36 + asan-ubsan 36/36 (incl. `viewer_unit` 8/8 and
  `viewer_e2e` live runs), cross-aarch64 11/11 with viewer/SSL off.

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

### Added (Phase 1B Block 3 — docs, fallback fixture)
- `docs/architecture.md`: proxy data-flow, routes, env injection (incl.
  the load-bearing `/v1` in the OpenAI bases), timing capture, redaction
  boundaries, thread ownership; stale "epoll forwarder" line corrected.
- `docs/trace-format.md`: `llm.request`/`llm.response` field tables,
  `llm/` blob + `.idx` formats, `truncated` semantics, turns counting;
  records the deliberate deviation from the AGENTS.md §3 sketch (no
  `llm.chunk` events, no `blob/usage/cost` fields — timing in `.idx`,
  parsing in the viewer).
- `docs/limitations.md`: proxy capture chapter (base-URL ignorance,
  HTTPS_PROXY-only SDKs, HTTP/2, deferred `--mitm`, uninjected
  Azure/Ollama, upload buffering).
- `docs/adr/0004-proxy-transport-httplib.md`: cpp-httplib streaming over a
  custom epoll forwarder, with consequences (incl. the 22.04-tsan regex
  finding and the per-request setup cost the latency test pins).
- `test/fixtures/real/toy-agent-3turn/` (FALLBACK, explicitly labelled):
  125 normalised events + 9 blobs from the toy agent — no
  `ANTHROPIC_API_KEY`/`claude` on the box, so the real-recording issue
  stays open. Secret grep empty.

### Fixed (Phase 1B Block 3 — D1 adversarial review, builder==reviewer)
- High: injected `OPENAI_BASE_URL`/`OPENAI_API_BASE` lacked `/v1`, so every
  real OpenAI-conformant SDK call misrouted to `api.openai.com/chat/...`
  (404) — invisible to tests (mock matches substrings; the agent used an
  explicit `/v1`). Bases now end in `/openai/v1`, clients use SDK-shaped
  paths; proven live (mock received `/v1/chat/completions`); fixture
  regenerated. Found by reading `resolve()` against openai-python's
  `{base}/chat/completions` convention.
- Medium: the upstream worker lambda captured `this` for two timeout
  longs; the shutdown-during-head-wait detach path could outlive the
  `LlmProxy` (use-after-free on `opts_`). Timeouts are now snapshotted by
  value; the lambda is `this`-free (clang-format reindented the body).
- Not fixed (documented): `--upstream` base paths are dropped (prefix
  gateways misroute, loudly); blob `ofstream` opens/writes unchecked
  (disk-full → silently missing blobs, no error channel in the schema);
  dead `!haveTtfb && bytes > 0` clause; `base64Decode` accepts len%4==1
  (fail-closed to 404). Full table in the Block 3 summary.

### Added (Phase 1B Block 2 — mock + toy agent + proxy integration)- `test/mockllm/server.py` (stdlib `ThreadingHTTPServer`): OpenAI Chat
  Completions + Anthropic Messages, streaming (manual chunked framing) and
  non-streaming, FIFO JSON scenario scripts (bodies verbatim, incl. tool
  calls; `gen_bytes`/`chunks_gen` emit deterministic multi-MB bodies without
  multi-MB files), per-response `chunk_delay_ms`/`delay_last_ms` overrides,
  verbatim headers log + `--sent-dir` req/res bodies for byte-exactness
  asserts, `GET /test-data`.
- `examples/toy-agent/agent.py` (~120 lines, stdlib): OpenAI-format tool
  loop (`write_file`, `run_command` via `sh -c`, `http_get` truncated to
  2000 chars); reads `OPENAI_BASE_URL`/`OPENAI_API_KEY`; deterministic
  3-turn dialogue driven entirely by the mock scenario.
- 8 CTest integrations (`test/proxy/`, stdlib only, own mock per test, no
  keys): toy_agent (3 pairs + exec/open/connect interleave, REDACTED
  stored vs real `Bearer` at mock, `3 LLM turns` epilogue), latency
  (first byte ~10 ms vs 2 s last chunk; median TTFB delta 1.2 ms debug),
  concurrent (8 streams byte-exact multiset sha256), disconnect
  (`truncated:true`, next 200), errors (429+500 passthrough, `0 LLM
  turns`), large (50 MB up intact, ~52 MB streamed down byte-exact),
  redact_argv (argv+manifest REDACTED, secret grep of run dir empty),
  anthropic (`x-api-key` REDACTED/stored, intact forwarded).
- CI `build-test` matrix gains the `tsan` preset (proxy thread coverage).
- `bench/ptrace_baseline.sh`: `strace -f -qq -o /dev/null` reference
  column, median traced event counts, `strace` version in the header;
  `bench.yml` installs `strace`. Local results regenerated (snowglobe is
  at/below strace on 3/4 workloads); the CI file regenerates via the bench
  workflow on push.

### Fixed (Phase 1B Block 2)
- Tracer `run.meta` cmd bypassed secret redaction (manifest cmd was
  clean) — caught by the new redact_argv e2e (secret bytes in
  events.jsonl). `ptrace_tracer.cpp` now runs `redactText` over `run.meta`
  cmd with `secretEnv_`; goldens unchanged (no secret-like argv there).
- 22.04-tsan `proxy_concurrent` failure (CI run 36447650097, exit 66):
  concurrent first-use construction of httplib's thread_local
  response-line regexes (one per upstream thread) races inside
  libstdc++-11's regex compiler (`_Compiler`/`_Scanner`/`ctype::narrow`
  on libstdc++.so globals). Both stacks are third-party frames — the only
  snowglobe frames are `cli.send` and thread spawn — our code has zero
  `std::regex`/`std::locale` use, and 24.04 (libstdc++ 13) is clean, so
  this is dependency+toolchain internal, not a proxy logic race.
  `test/tsan.supp` suppresses exactly those three frames (wired via
  `TSAN_OPTIONS` for the proxy tests in tsan builds only); unit, golden,
  kill and tracer tests keep full unsuppressed TSan coverage.
- Latency budget is preset-aware: the 5 ms product budget holds on plain
  builds (debug measures 1.2 ms); ASan slows the proxy ~6 ms, so sanitizer
  presets bound the delta at 25 ms via `SG_LAT_BUDGET_S` (set in
  test/CMakeLists from the sanitize flags). Same test, still a bound.

### Fixed (Phase 1B Block 1 CI, run 36332355926 + follow-up run 36427541348)
- `cross-aarch64`: host x86_64 OpenSSL cannot be used by
  `aarch64-linux-gnu-g++` (`openssl/opensslconf.h` is arch-specific, then
  host libs mismatch). Added `SNOWGLOBE_WITH_SSL` (default ON); the cross
  job passes `-DSNOWGLOBE_WITH_SSL=OFF` (no `find_package(OpenSSL)`, no
  `OpenSSL::` link, httplib with both `HTTPLIB_REQUIRE_OPENSSL=OFF` and
  `HTTPLIB_USE_OPENSSL_IF_AVAILABLE=OFF` so `CPPHTTPLIB_OPENSSL_SUPPORT` is
  never defined — the second switch matters because CI has `pkg-config`,
  which lets httplib auto-find host OpenSSL even with `REQUIRE` off).
  Native jobs keep TLS; the cross job stays build-only. Verified with
  `pkg-config` installed (CI-like): cross proxy compile has no
  `CPPHTTPLIB_OPENSSL_SUPPORT`, build clean, `file core/snowglobe` = `ELF
  64-bit LSB pie executable, ARM aarch64`; native still defines it.
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

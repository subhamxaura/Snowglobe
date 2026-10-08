# Changelog

All notable changes to this project will be documented in this file.
Format: Keep a Changelog. Versioning: SemVer (schema v0 until v0.1.0).

## [Unreleased]

### Added (Phase 3 Blocks 1+2 — baseline, diff, apply, compare; ADR-0008)
- `run --isolate` snapshots a hashed `baseline.json` first (regular files
  + symlinks; skips `.git/`, `.snowglobe/`, `node_modules/` +
  `--baseline-exclude`; manifest gains `"baseline":"baseline.json"`).
  Walk cost is logged (`note: baseline N files in Mms`).
- `snowglobe diff <run> [--stat] [--patch=FILE]`: pending change set from
  every overlay upper (A/M/D/R + S inventory; R = whiteout + same-hash
  add, non-empty files only; already-applied entries suppressed but
  counted). Real Hirschberg-diff hunks (host bytes verified vs baseline;
  mismatch → letter kept, hunks withheld). Non-isolate runs get an honest
  event-derived path report, never hunks. Exit 0; 69 without
  baseline/upper.
- `snowglobe apply <run> [--dry-run] [--yes]`: validate-all-then-write
  (conflict = host ≠ baseline; safety: absolute/`..`/`.git`/parent-chain
  symlinks refused, temp+rename writes, dir deletes need exact subtree
  match). Any conflict/rejection → exit 65, NOTHING written. `--dry-run`
  reports identically; off-tty requires `--yes`.
- `snowglobe compare <runA> <runB> [--stat] [--json]`: host-independent
  change-set join (A-only/B-only/both-same/both-differ).
- Tests: Myers/Hirschberg property tests (500 randomized reconstructions
  + optimality oracle, exhaustive small cases, budget fallback) and
  baseline round-trip/strict-reader units; `diff_apply` golden (scripted
  isolate ops, stat/patch/dry-run/compare/event-mode goldens, clean
  apply + rerun-empty, conflict + extras + symlink-escape 65 with
  byte-identical zero-writes proofs, exit codes 64/65/69, crafted dir
  whiteouts). Known kernel behavior: agent `rmdir` of an emptied merged
  dir fails EIO here (per-file whiteouts work; see limitations).

### Fixed (CI: prlimit scoping vs ASan observers)
- `RLIMIT_AS` (and NPROC/NOFILE) moved from the isolate middle to the
  agent child just before `execvp`: the 22.04-asan CI leg died with
  `AddressSanitizer failed to mmap` under explicit `--memory-max=64M`
  (ASan needs vast address space; observer must stay unlimited). Limits
  still reach the agent (`ulimit -u/-n/-v` pinned); middle/init never
  limited. AS-limit test now uses absolute `/bin/bash` (no PATH-search
  allocator activity pre-exec on old toolchains).

### Fixed (Phase 3 D1 self-review, core/diff+apply builder==reviewer)
- [High] `planApply` treated non-ENOENT `lstat` failures as absent (EACCES
  would stage writes that fail mid-apply, breaking all-or-nothing): new
  `statFinal` helper aborts validation (70, zero writes) on any IO error.
- [High] Directory deletes checked baseline files but ignored host extras
  (extra files would vanish silently in the rmtree): new `checkSubtree`
  demands an exact match (content, targets, no extras) or conflicts.
- [Medium] Symlink policy was blanket-reject (broke legitimate retargets
  and masked a vacuous escape test): per-kind validation now (parent
  traversal still refused; temp+rename never follows).
- [Medium] Unknown-content deletes never suppressed on missing host
  (rerun-after-apply never emptied): missing host path now counts applied.
- [Low] Stat hid counts on link rows; `close(-1)` on a failed dup; dead
  helpers left from refactors. TOCTOU/O_NOFOLLOW discipline documented
  in-code. Second-model ranges: `core/diff/apply.cpp:32-115`,
  `:189-290`, `:364-754`.

### Fixed (Phase 3 ITEM 0 — second-model review findings, all cited with severity)
- [High] x32 ABI bypassed the seccomp filter (R5): x32 numbers reuse
  `AUDIT_ARCH_X86_64` with bit 30 set, missing every nr-JEQ into ALLOW
  (proven: x32 `add_key` returned a live key serial under the old filter;
  `probe/x32.c` prints ENABLED on `CONFIG_X86_X32_ABI=y` kernels).
  The filter now denies `nr >= 0x40000000` first (JGE→EPERM), with a
  regression test (`seccomp x32 numbers are denied`).
- [Medium] `argvMatches` over-matched substrings (R2): tool command "rm"
  upgraded `proc.exec ["sh","-c","perform_clean"]` to argv-match. Joined-argv
  hits now require token boundaries (start/end, whitespace, `/`); pinned by
  `argv-match requires token boundaries` (negative + `/bin/rm` and
  `git status --short` positives).
- [Low] Linker span prose disagreed with the res-partition implementation
  (R1): `link.hpp`, the Pass-2 comment, `docs/limitations.md`, and ADR-0006
  (+R1 note) now state `[response N, response N+1)`; new in-flight test pins
  `[req_{N+1}, res_{N+1})` → turn N. No format change (fixtures byte-identical).
- [Low] Missing monotonicity assert on the `spanAt` binary-search invariant
  (R3): turns sorted by `reqKey` now assert non-decreasing `resKey`
  (concurrent out-of-order completion trips loudly instead of misattributing).
- [Low] BPF jump offsets were unchecked 8-bit truncations (R4): all forward
  jumps go through a range-asserted helper.
- [Info] No `SECCOMP_FILTER_FLAG_TSYNC` rationale recorded (R6): installer
  documents the single-threaded pre-fork invariant it relies on.

### Added (Phase 3 ITEM 0)
- `docs/threat-model.md`: the AGENTS.md §1.4 promise — microVM-vs-isolate
  boundary, accident-containment framing, explicit non-goals, boundary table.
  §1.4 conformance is now fully RESOLVED (this doc was the open item).

### Added (Phase 2 Block 3 — conformance resolution; closes Block 2 docs)
- Seccomp Docker parity (moby/profiles `seccomp/default.json`, unprivileged):
  ERRNO(EPERM) on add_key/keyctl/request_key, clock_settime/settimeofday/stime,
  create/delete/query_module, get_kernel_syms, sysfs/_sysctl/uselib/ustat,
  ioperm/iopl, io_uring_enter/register, kcmp/process_vm_readv/writev,
  lookup_dcookie/pidfd_getfd, get_mempolicy/mbind/move_pages/set_mempolicy,
  nfsservctl, quotactl, swapoff, chroot/setdomainname/sethostname/syslog/vhangup,
  fsconfig/fsmount/fsopen/fspick/mount_setattr/move_mount/open_tree/fanotify_init,
  setns, umount; ENOSYS on clone3 (glibc falls back to clone); KILL stays on
  io_uring_setup (strict superset, documented). Arg-filtered (BPF): clone/unshare
  deny namespace creation (threads/processes keep working), socket denies
  AF_ALG(38)/AF_VSOCK(40) only, personality allows 0/8/131072/131080/0xffffffff.
  ptrace/process_vm/kcmp + NUMA stay blocked as strict supersets (documented).
- prlimit fallback (always in-child; cgroup adds on top when delegated):
  RLIMIT_NPROC=512 (`ulimit -u`), RLIMIT_NOFILE=1024 always; RLIMIT_AS only when
  `--memory-max` explicit (`ulimit -v` finite, else unlimited — AS breaks Bun).
  Run log notes `(prlimit fallback active: ...)`; `doctor` shows
  `cgroup: no (prlimit fallback active)` when delegation denied.
- Default secret-path masks (tmpfs 0700): `~/.ssh`, `~/.aws`, `~/.gnupg`
  (empty inside; host keys invisible); `--allow-path PATH` (repeatable, absolute,
  exact-or-parent) exempts. `run.meta.masks[]` + manifest `isolate.{masks,allow_path}`
  record the choice. ssh-based git remotes need `--allow-path ~/.ssh` (documented
  in architecture/limitations).
- `doctor --isolate` procfs row: `optional — REQUIRED for Bun/Node-class runtimes
  (issue #4); empty-tmpfs fallback active`. cgroup-deleg row: `cgroup: no
  (prlimit fallback active)` when denied.
- Canonical overlay uppers (implementation truth): project `overlay/upper`
  (diff/apply source), `/etc` `overlay/etc-upper`, `$HOME` `overlay/home-upper`,
  `--fs-rw` `overlay/fs-rw-N`. Historical `system-upper` wording retired (ADR-0007
  Block 3 note); `fuse-overlayfs` DEFERRED (native required); namespaces via
  `clone3` or `unshare(2)` (§1.4 amended 2026-10-05).
- Tests: unit arch-mismatch KILL (fail-closed, no fail-open), arg-filtering
  allow/deny (clone NS, socket ALG/VSOCK, personality values, clone3 ENOSYS),
  blocklist shape (Docker additions); integration `isolate_basic` k/l/m (seccomp
  add_key/ALG/getmem EPERM inside vs success outside + personality query allowed;
  prlimit `ulimit -u/-n/-v` + AS explicit; masks empty/exempt + run.meta).

### Added (Phase 2 Block 2 — restrictions; partial, Block 3 closes docs)
- seccomp allowlist-inverse (raw BPF, no libseccomp): ERRNO(EPERM) on a
  curated blocklist (mount/umount/pivot_root/kexec/reboot/modules/ptrace/
  bpf/perf/userfaultfd/swapon/acct/open_by_handle_at), KILL_PROCESS on
  `io_uring_setup` (retires `UV_USE_IO_URING=0` under `--isolate` only).
- Landlock ABI≥1 (shared helper, also used by doctor): RO `/`, RW islands
  (repo, HOME, /tmp, /etc, /dev, /run, `--fs-rw`); applied last, child
  only. Denials surface as `fs.open ok:false errno:13` (EACCES by design).
- `fs.open` gains `"ok":false,"errno":N` on failure only (success lines
  and goldens byte-identical).
- Env masking (`KEY|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIAL$`,
  `--allow-env` re-admits, `*_BASE_URL` always passes); `$HOME`
  overlaid writable by default; `--fs-rw PATH` (repeatable, dirs only).
- cgroup v2 best-effort (`--memory-max/--pids-max`, 2G/512 defaults);
  delegation denied proceeds with a note (this box).
- `doctor --isolate` (ISOLATE rows from real probes, exit 0/69);
  `run --isolate` setup failures echo the same table.
- `scripts/check-format.sh` owns the format gate (CI lint calls it).

### Added (Phase 2 Block 1 — isolate runner)
- `snowglobe run --isolate`: unprivileged userns (parent-written id
  maps) + mount + pid namespaces, selective overlays (project →
  `<run>/overlay/upper`, `/etc` → `overlay/system-upper`), ro binds,
  empty-tmpfs `/proc`, tmpfs `/tmp`, pivot_root, PID-1 reaper with exit
  propagation. `run.meta` gains `"isolate":true`; manifest gains
  `isolate:{on,features,upper}` (non-isolate output byte-identical).
  Setup failures name step + errno and exit 69 (ADR-0007).
- `isolate_basic` + `isolate_toy` CTests (repo + /etc copy-up with host
  clean, proc invisibility, 3-turn toy shape under isolate).
- `doctor` overlayfs-in-userns check uses the parent-map dance (honest
  green); `viewer` gains a `typecheck` script (local tsc, never bare
  npx).

## [v0.1.0-alpha.4] - 2026-10-05

### Added (Phase 1D Block 2 — ground truth + viewer single-sourcing)
- `GET /api/links`: serves `links.json` when the run has one (read per
  request, no ETag), else 404 and the viewer falls back visibly
  (`view_api.py` covers both).
- Viewer single-sourcing (`TurnIndex` + `links.json`): sidecar exact
  membership wins where it speaks, span heuristic elsewhere; turn
  detail shows basis badges + unattributed reasons, header reads
  `linkage: sidecar|heuristic`. Playwright covers both paths
  (sidecar: argv-match badge + note.txt[window]; fallback labelled).
- Ground-truth pins: toy-agent-3turn EXACT sets/bases/tools in
  `test_link_fixtures.cpp` (curated-small rationale); claude-code-1-error
  exact ids + sizes + small-turn sets + partition/reason/vocab invariants
  (190/521 split, all pre-turn). `model.test.ts` sidecar unit tests.
- `scripts/check-format.sh` (C/C++ only — clang-format mangled a `.py`
  in Block 1) now owns the gate; CI lint calls it.
- Docs: `trace-format.md` links.json section, `architecture.md` linker
  paragraph + `/api/links`, `viewer.md` badges/fallback,
  `limitations.md` anchoring difference.
- D1 over `core/link/` (builder==reviewer): 0 High, 1 Medium (duplicate
  React keys on seq-less fixtures) + 1 Low (stale-sidecar null) fixed;
  second-model review of `linker.cpp` requested separately, not blocking.

### Added (Phase 1D Block 1 — linker core)
- `core/link/` + `snowglobe link <run> [--check]`: causal attribution
  as a derived `links.json` sidecar (ADR-0006; events never rewritten).
  Rules window/lineage/argv-match with basis + confidence, explicit
  `unattributed` reasons, probes excluded; deterministic output,
  `--check` exits 3 when stale.
- Catch2 `test_link_scan`/`test_link` (field scanner, OpenAI/Anthropic
  tool extraction, per-rule synthetic attribution incl. cross-boundary
  lineage) + `link_determinism` CTest (double-run byte-compare on
  toy-agent-3turn and claude-code-1-error copies).
- ADR-0006 (derived link sidecar + `link` CLI amendment).

### Added (Phase 1C Block 2/3 — real fixture, providers split, error views)
- `test/fixtures/real/claude-code-1-error/`: REAL Claude Code recording —
  735 events, 0 `trace.decode_error`, HEAD probe + 11 turns all HTTP 401
  with real Anthropic error envelopes; proxy-redacted headers
  (`x-api-key: REDACTED`, no Authorization stored), normalised paths
  (`$REPO`/`$HOME`/`PORT`), re-runnable secret audit in the fixture
  README. Success-path real recording deferred → issue #2 (needs API
  credits); docs/viewer.md states the coverage split explicitly.
- `viewer/lib/providers/` split (`openai.ts`, `anthropic.ts`, `sse.ts`,
  `pricing.ts`, `types.ts`; `providers.ts` stays the public facade) and
  a new `errorInfo(provider, body)` that extracts Anthropic
  `{"type":"error","error":{type,message}}` / OpenAI `{"error":{...}}`
  envelopes without ever throwing.
- Turns view: error turns render `HTTP <status> · <type>: <message>` +
  a **response body** raw toggle; pair building now single-sources
  `buildTurns`, so the HEAD pre-flight no longer renders as a phantom
  first turn on real traces (caught by the real fixture).
- Playwright `error.spec.ts` real-fixture smoke: 735 events, status +
  envelope assertions, raw-body toggle opens, probe never a turn, no
  blank screen, zero non-localhost requests. Screenshots
  `docs/screenshots/turns-{toy-agent,error-401}.png` refresh via the
  opt-in `SG_SCREENSHOT_DIR` (CI never writes into the repo).
- `viewer/test/real-error.test.ts`: turn layer + Anthropic error
  parsing against the real recording (vitest 20/20).
- `test/fixtures/real/claude-code-1-nocredit/`: second REAL Claude Code
  recording, salvaged from the regenerated no-credit capture
  (`~/claude-real.sgr`, post-/tmp-wipe): 566 events, 0
  `trace.decode_error`, HEAD probe (502) + 1 real turn HTTP 400 with a
  193-byte BINARY body stored verbatim — the proxy
  format-agnosticism proof. `real-error.test.ts` pins it (1 error
  turn, probe excluded, binary fold empty / `errorInfo` null, redacted
  headers; vitest 25/25). Issue #2 stays open — only the success path
  is still missing.

### Fixed
- view `/api/summary`: an `llm.request` whose `model` key is *absent*
  read as "has model", counting bodyless model-less POSTs as turns and
  diverging from `model.ts` `isProbeRequest` — an absent key now reads
  as no model (`view_api` regression check added).
- view `/api/events`: `to < from` now returns 400, matching the error
  message the API always printed.
- `costUsd()`: own-property lookup — hostile trace model ids
  (`"constructor"`, `"toString"`) can no longer resolve through
  `Object.prototype` and render `$NaN`.

### Security / hardening
- D1 self-review (**builder == reviewer**, re-run with a second model
  per Part E) over `core/view/` + `viewer/lib/`: 0 High; 1 Medium
  (probe-rule mirror divergence, fixed above); 2 Low (bounds check +
  prototype-key pricing, both fixed); remaining findings Info and
  documented (summary memory footprint, first-match field scanning
  scope, shared weak ETag mitigated by `no-store`, torn-trace load
  erroring loudly rather than truncating silently).


### Added (Phase 1C Block 2 un-gated slice — pricing + docs)
- `viewer/lib/pricing.json`: single price table ($/1M tokens) with a
  `verified` date per model (checked against the providers' official
  pages on 2026-10-03; sources listed in-file) and current-gen Claude
  entries (fable-5-1, opus-5-5, sonnet-5-5, haiku-4-5). n/a is absence,
  never 0: unpriced models (e.g. the mock LLM's `mock-model-1`) render
  as no cost, never $0. `providers.ts` reads the table; new
  `priceVerified()` exposes the date. Vitest pins the table shape
  (15/15).
- `docs/viewer.md`: viewer architecture doc (serving model, turn rules,
  providers/pricing, build/embed, testing).
- Binary size vs embed recorded in STATUS.md: debug binary 10,469,168 B
  (viewer off) vs 11,420,896 B (embedded, 12-file gzip table
  392,988 -> 127,376 B stored).

### Added (Phase 1C Block 1 — view server + model + embed)
- `snowglobe view <run|events.jsonl>`: `/api/manifest` (synthesized for
  bare `.jsonl`), seq-indexed `/api/events?from=&to=` (≤5000/page: 400
  beyond, 416 past end), `/api/blob/<path>` confined to the run dir
  (lexical `..` reject + canonical symlink containment; escapes read as
  404), `/api/summary` (kinds, turns with probes excluded and errors
  included, processes, tcp/unix hosts + disconnects, files, duration);
  `/trace/*` stays as a blob compat alias. ETags + 304 on APIs and
  embedded assets; immutable cache for hashed assets; `--open` never
  fails without xdg-open (prints a note, keeps serving).
- Embedding is gzip: `cmake/embed_viewer.py` (stdlib only) compresses
  each file (`mtime=0`), records sha256 ETag + MIME + immutable bit;
  `SNOWGLOBE_BUILD_VIEWER=ON` (default ON, `SNOWGLOBE_VIEWER=OFF` still
  honoured) with a loud degrade-to-OFF when node/npm are absent.
- `viewer/lib/model.ts`: schema-0 types + turn layer (`getTurnForEvent`
  stable; error turns count; HEAD/model-less-bodyless probes excluded;
  `net.disconnect` + structured family/ip/port/path with legacy addr
  fallback); `viewer/lib/load.ts` pages `/api/events` (legacy
  `/trace/events.jsonl` fallback); ADR-0005; `view_api` CTest.
- `docs/limitations.md`: WSL interop section no longer names the lost
  error-path fixture (regeneration via an invalid-key 401 run noted).

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

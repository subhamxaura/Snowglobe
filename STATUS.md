# STATUS — Snowglobe
Updated: 2026-10-07  ·  Phase 2 Block 3 DONE (unpushed): §1.4 amended (4ee0c15, human approval 2026-10-05) + conformance (seccomp Docker parity + arg filtering, prlimit fallback, secret-path masks + --allow-path, doctor procfs/cgroup text, canonical overlay paths, ADR-0007 Block 3 note, trace-format/architecture/limitations docs, AGENTS §3 overlay path) + tests (unit 4 new: arch-KILL fail-closed, clone/socket/personality allow+deny, clone3 ENOSYS, masks pure; integration k/l/m: add_key/ALG/getmem EPERM inside vs success outside + persq query allowed, prlimit ulimit -u/-n/-v + AS explicit, masks empty/exempt + run.meta) + D1 builder==reviewer (0H/2M fixed: socket-VSOCK fail-open jf=1->0, personality jt miscalc; L/I documented; seccomp core/isolate/seccomp.cpp:302-491 + linker core/link/linker.cpp:182-199,313-395 for second-model review) + gates: ctest debug 65/65 + asan-ubsan 65/65 (zero findings), vitest 28/28, tsc clean, format clean (clang-format applied), Playwright in-suite (viewer_e2e green)  ·  Conformance table (§1.4 → status): namespaces clone3-or-unshare RESOLVED (unshare impl, net host per ADR); overlay upper canonical RESOLVED (overlay/upper|etc-upper|home-upper|fs-rw-N, native required, fuse DEFERRED); ro-bind/tmpfs RESOLVED (Block 1); secret-path masks RESOLVED (env Block 2 + path tmpfs Block 3 + --allow-path, run.meta/manifest, ssh remotes doc); seccomp Docker parity RESOLVED (EPERM + clone3 ENOSYS + io_uring KILL superset + arg filtering, ≥3 proven + arch fail-closed); landlock RESOLVED (Block 2); cgroup/prlimit RESOLVED (best-effort + NPROC/NOFILE always + AS explicit, doctor row, ulimit test); never-secure-sandbox RESOLVED (source grep 1 line, ADR reworded, fixtures verbatim excluded); microVM doc PENDING (docs/threat-model.md absent, Phase 2 remaining; limitations/architecture carry the non-boundary note)  ·  Alpha.5 note (pre-tag): isolate + Bun-class runtimes blocked (issue #4; empty /proc SIGABRT, 0 turns); Python-class agents work (toy 3-turn + sh/bash/python helpers green). Real gate re-runs when #4 resolves — stays open.  ·  Issue #4 commented (RO rbind host /proc as opt-in --proc=host, discussion only, not implemented).  ·  Secure-sandbox grep: source-only 1 line (AGENTS.md prohibition); fixtures contain the line as recorded prompt evidence (verbatim, excluded).
Updated: 2026-10-07  ·  Phase 2 Block 2 DONE (pushed: 4b84409 + e591987 + fixes): seccomp (raw BPF, EPERM list + io_uring KILL), landlock (shared helper, RO/RW), env masking + allow-env, $HOME overlay + --fs-rw, cgroup best-effort (denied here → note), doctor --isolate (all req green, exit 0), fs.open ok/errno on failure  ·  Gates: ctest debug 61/61 + asan-ubsan 61/61 (x2), vitest 28/28, typecheck, format (gate now covers untracked), Playwright in-suite; CI green https://github.com/subhamxaura/Snowglobe/actions/runs/37639852457 (9/9; fixed lint/format, 22.04 landlock ifndefs, aarch64 stringop false-positive, SIGPIPE→clean-errors, EPIPE mask)  ·  Real-runtime gate BLOCKED: claude (Bun) SIGABRTs on empty /proc (0 turns; issue #4 has the evidence) — follow-ups decided from the denied list  ·  Flake hunt: asan middle stuck in futex (inherited-lock class) → spawn-before-threads + 60s handshake timeouts; full asan suite green since.
Prior (1D closed 2026-10-05): tag v0.1.0-alpha.4 → 53aec54, tag-tree CI green; issues #2 (success fixture) + #3 (Timeline lanes) open; second-model linker.cpp review outstanding (Part E rule 3); branch protection still human-action.
## Phase 1D — causal linking (link CLI contract, rules, test policy)
- Contract: `snowglobe link <run> [--check]` (ADR-0006 amends AGENTS.md §2);
  `links.json` derived sidecar, never rewrites hash-chained events;
  `--check` exits 3 when stale/missing. `GET /api/links` serves it (404 →
  labelled heuristic fallback).
- Rules: `window` (response-anchored `[res_N, req_N+1)`), `lineage`
  (birth-turn retention, pid-reuse/thread/vanished handling), `argv-match`
  (tool command in exec argv, upgrades only); probes + `llm.*` skeleton
  excluded; pre-turn bucket with reasons; confidence `high` throughout.
- Fixture-test policy: toy-agent-3turn EXACT sets (curated-small);
  claude-code-1-error exact ids/sizes/small-sets + partition/reason/vocab
  invariants (190-seq paste unreadable — rationale in test file);
  determinism by double-run byte-compare; viewer both-paths Playwright.
- Anchoring difference (response- vs request-anchored) documented in
  `docs/limitations.md`; sidecar wins where it speaks.
- Reviews: D1 builder==reviewer done (0H/1M/1L fixed). Second-model
  `linker.cpp` review still outstanding (Part E rule 3) — requested, not
  blocking; must land before Phase 2 leans on attribution.
- Branch protection: NOT enabled (force-push still possible) — human
  action required in repo settings; AGENTS.md §6.8 is policy only.
Prior (1D Block 2): /api/links + viewer single-source (badges, sidecar/heuristic label, both-paths Playwright), toy exact + error sizes/partition ground truth, check-format.sh owns gate, docs (trace-format/architecture/viewer/limitations), D1 (0H/1M/1L fixed; second-model linker.cpp review REQUESTED separately — not blocking)  ·  Gates: ctest debug 53/53 + asan-ubsan 53/53, vitest 28/28, tsc, format script, Playwright 4 specs incl. both-paths  ·  Block 3 (gates+tag alpha.4) is a separate session — DO NOT start here.
Prior (1D Block 1): core/link/ + `snowglobe link [--check]` (ADR-0006), Salvage DONE: claude-code-1-nocredit/ 2nd error fixture (566 ev, 0 decode_error, probe 502 + 1×400 with 193 B binary body verbatim, secret audit 0; vitest 25/25) — issue #2 stays open, only success path missing  ·  Gates re-run: ctest debug 44/44 + asan-ubsan 44/44 (zero findings), vitest 25/25, e2e in-preset green, tsc + clang-format clean  ·  Phase 1C Block 2+3 DONE (skip-credits adjudication): real error fixture claude-code-1-error/ (735 ev, 11×401, secret audit 0), providers/ split + errorInfo, error views, Playwright smoke, screenshots, D1 (builder==reviewer)  ·  Success-path real fixture DEFERRED → issue #2  ·  Tag v0.1.0-alpha.3  ·  2026-10-04: origin/main force-updated f93434b→c0623ad 18:14Z by actor subhamxaura (NOT this session; which session undetermined); rule in AGENTS.md §6.8 (never force-push main, one session per tree, rewrites need approval + refetch)

## Block 1 DONE (server /api + model.ts + gzip embed + ADR-0005)
- [x] `view`: /api/manifest|events(paged ≤5000: 400/416)|blob(confined: `..` + symlink escape → 404)|summary; bare .jsonl (synthesized manifest); ETag/304 everywhere, immutable hashed assets; --open never fails w/o xdg-open
- [x] embed: `cmake/embed_viewer.py` gzip table (12 files, 392461 → 127292 B stored, −68%), MIME/ETag/immutable; SNOWGLOBE_BUILD_VIEWER=ON (VIEWER=OFF alias kept); binary debug 11494584 → 11420808 B (−74 KB), viewer/out 448K → 452K
- [x] `viewer/lib/model.ts`: schema-0 types + TurnIndex.getTurnForEvent stable; error turns count; probes (HEAD or model-less+bodyless) excluded; disconnect + structured net fields + legacy addr fallback; `load.ts` pages /api/events at 5000 (legacy /trace fallback)
- [x] ADR-0005; architecture updated; `view_api` CTest green; lint clean (`git ls-files '*.cpp' '*.hpp' '*.h' '*.c' | xargs clang-format --dry-run --Werror`)
- Verification (WSL2, uid 1000, ext4): debug 44/44 + asan-ubsan 44/44 (zero findings); vitest 14/14 (6 model + 8 providers); e2e 2/2 (incl. 50 MB < 3 s after 5000-page fix; one 3021 ms flake at 1000/page motivated it)
- Block 2 DONE (skip-credits): `providers/` split (openai/anthropic/sse/pricing/types + facade) + `errorInfo()`; Anthropic error parsing tested against claude-code-1-error/ (real envelope/headers/status) PLUS in-tree mock scenarios for success content blocks + streaming; OpenAI parser stays on toy-agent + mock; docs/viewer.md carries the explicit no-overclaim note (success fixture pending issue #2). Un-gated slice (pricing.json, n/a≠0) as before; vitest 20/20.
- Block 3 DONE: error views (HTTP status + envelope banner, raw response-body toggle; Turns single-sources buildTurns so the HEAD probe never renders as a turn — caught by the real fixture), Playwright error smoke `error.spec.ts` (735 events, 401+envelope, toggle opens, no blank screen, offline) + toy-agent success flow + 50 MB perf, screenshots in docs/screenshots/ (SG_SCREENSHOT_DIR opt-in), D1 over core/view + viewer/lib (builder==reviewer: 0 High, 1 Medium + 2 Low all fixed, rest Info documented), CHANGELOG updated.
- Block 3 (later): Playwright smoke on real fixtures, screenshots, v0.1.0-alpha.3. DONE early: offline assert (watchExternal fails on any non-localhost request), docs/viewer.md.
- Binary size vs embed (spec item, debug, WSL2 24.04): SNOWGLOBE_BUILD_VIEWER=OFF 10,469,168 B -> ON 11,420,896 B (+951,728 B = gzip table of 12 files, 392,988 -> 127,376 B stored, -67.6%; decimal-escaped arrays cost ~7.5x the stored bytes).
- ADR-0002: slot never used in any history; stub 0002-unused-slot-see-note.md added so numbering is not dangling.

> Phase 1A gate closed: CI run
> https://github.com/subhamxaura/Snowglobe/actions/runs/36311530345 — all 6
> jobs green (22.04 + 24.04 × debug + asan-ubsan incl. tcp_loopback,
> cross-aarch64, lint), plus bench workflow run
> https://github.com/subhamxaura/Snowglobe/actions/runs/36311530354
> (success, committed bench/results/ptrace-baseline-ci-24.04.md separately).
> Tag: v0.1.0-alpha.1. Phase 1B is a separate session.

> Block 2 gate closed: CI run
> https://github.com/subhamxaura/Snowglobe/actions/runs/36301575125 — all 6
> jobs green (22.04 + 24.04 × debug + asan-ubsan, cross-aarch64, lint).
> Block 3 (bench + docs + real recordings) is a separate session.

> Phase 0 gate closed 2026-09-27 (CI run 36291693217, all 6 green). This
> session did Phase 1A Block 1 only (process-tree correctness); Blocks 2
> (goldens + kill test) and 3 (bench + docs + real recordings) are separate
> sessions. Schema stays 0 (all trace changes are additive optional fields).

## Block 2/3 verification (2026-10-03, WSL2 uid 1000, all real output)
- Fixture: 735 events (0 decode_error), 12 llm.request/response, 1 HEAD
  probe (502 "upstream failed") + 11 turns status 401 (real
  authentication_error envelope), 36 blobs, 11×100 KB identical system
  prompts (git dedupes to one); audits: sk-ant/Bearer 0, x-api-key
  REDACTED ×11, /home/tester 0, only noreply@anthropic.com (prompt text).
- Gates: `ctest --preset debug` 44/44 (37.2 s), `asan-ubsan` 44/44
  (47.2 s, zero findings), vitest 20/20, Playwright 3/3 (view + error +
  perf, <10 s), `tsc --noEmit` clean, `clang-format --dry-run --Werror`
  clean.
- D1 (builder==reviewer, core/view + viewer/lib): [M] absent-model key
  read as "has model" in summary probe rule → fixed + view_api check;
  [L] `to < from` unenforced → fixed + check; [L] prototype-key pricing
  → $NaN fixed + tests; Info: summary holds trace in memory once
  (fine ≤50 MB), valueAt first-match scanning scope documented,
  shared weak ETag mitigated by no-store, torn trace loud-errors in
  paged loader (legacy path skips), types overstate required fields.

## Done (this phase)
- Repo skeleton: CMake ≥3.25 + Ninja, presets (debug/release/asan-ubsan/tsan),
  clang-format (LLVM/100), clang-tidy, editorconfig, gitignore, Apache-2.0 LICENSE
- `ITracer` + ptrace backend (fork/vfork/clone/exec, path canonicalisation,
  open/unlink/rename/mkdir/connect/sendto/bind decode, default filters, decode_error)
- `JsonlWriter` (SHA-256 chain, per-event flush) + manifest lifecycle
- `snowglobe run|doctor|ls|rm|version` (+ honest not-implemented stubs)
- Unit tests (json escaper, sha256 vectors, hash-chain verify) + integration
  golden test (open→rename→unlink + chain integrity) + 6 probes + CI
- docs: PLAN.md, architecture.md, trace-format.md, adr/0001; README (pre-alpha);
  CHANGELOG.md

## In progress
- Phase 1D Block 3 close-out (this session): gates → push → CI green →
  tag v0.1.0-alpha.4. Next: Phase 2 (isolation) starts with a new prompt.
- Note: OpenSSL links dynamically for now (static is a Phase 4 problem).

## Verification (WSL2 Ubuntu 24.04, all real output)
- `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`:
  100% (4/4) — 3 unit + tracer_basic golden (open→rename→unlink + chain/manifest)
- `asan-ubsan` preset: build clean, 100% (4/4), zero sanitizer findings
- `snowglobe doctor`: kernel/userns/overlayfs/seccomp-notify/cgroup-v2/ptrace
  yes; landlock/pasta no (WSL2 kernel/userland limits — expected, degrades cleanly)
- `run -- python3 -c "open('/tmp/x','w').write('1')"`: 41 events incl.
  `proc.exec /usr/bin/python3` (full argv) + `fs.open /tmp/x write:true`;
  0 decode_error; manifest event_count/last_hash consistent
- `run -- sh -c 'ls | head -1'`: 2 child proc.exec (/usr/bin/ls, /usr/bin/head),
  both ppid == root pid — PIPE TEST PASSED
- `run -- sh -c 'exit 7'`: snowglobe exits 7 (propagation ✓)
- Kill test: `kill -9` supervisor mid-`sleep 100` → no orphan remains
  (PTRACE_O_EXITKILL verified) — KILL TEST PASSED
- 4 Linux-only bugs fixed (see CHANGELOG Fixed): syscall-info enum off-by-one,
  exec ENTRY caching, JSONL `}}`, environ linkage
- Doctor probes made real (no more guessing): Landlock ABI via ruleset VERSION
  query → **abi 3** (earlier /proc guess said absent — wrong); seccomp-notif via
  GET_ACTION_AVAIL → yes; overlayfs via unshare+uid_map+mount dance → yes;
  apparmor userns knob reported (absent on WSL kernel). Datasets: WSL2 6.6.
- Format clean (`clang-format --dry-run --Werror`), aarch64 cross-build clean,
  CI test step simplified to `ctest --preset ${{ matrix.preset }}`.

## Block 1 verification (WSL2 Ubuntu 24.04, all real output)
- `probe/threads.c` (4 pthreads × file open): 4 `proc.start thread:true` with
  pid=tgid + distinct tids; all 4 `fs.open` attributed to the right tid —
  THREAD-ATTRIBUTION-PASSED.
- `probe/mt_exec.c` (leader execs): worker death reported, one `proc.exec`,
  clean end. Non-leader exec (throwaway): vanished-tid exit + scavenged
  `proc.exec /bin/echo argv=[echo nonleader-exec-ok]`, 0 decode_error.
- Signals: SIGKILL root → exit 137 + finalised manifest; SIGTERM snowglobe →
  root TERM'd, drain, exit 143 + finalised; double Ctrl-C → SIGKILL tree,
  exit 70, partial manifest, no orphans.
- Filters: `find` 12 events default vs 43 with `-a`; `fs.rmdir`, `fs.symlink`,
  `fs.chmod mode=493`, `O_TMPFILE` → `path=/tmp write:true tmpfile:true`.
- vfork smoke: child exit 42, parent resumes, exit 0. `UV_USE_IO_URING=0`
  present in child env.
- ctest debug + asan-ubsan: 100% (5/5 incl. new open-flags unit test), zero
  sanitizer findings; clang-format clean.

## Block 2 verification (all as uid 1000 except builds; WSL2 Ubuntu 24.04)
- 6 goldens (262/28/22/59/23/38 events), byte-stable across repeat runs;
  14/14 debug (2.6 s) and 14/14 asan-ubsan (3.4 s), zero sanitizer findings.
- Kill tests: supervisor SIGKILL → no survivors in 2 s; root SIGKILL →
  exit 137 + finalised manifest; supervisor SIGTERM → exit 143 + finalised.
- CI run 36301575125 all green, incl. 22.04 (C helpers dodge CPython
  startup divergence; helpers build sanitizer-free — LSAN is fatal under
  ptrace — with $HELPERS normalised before $REPO).
- Env notes: work happens in ~/src/snowglobe (ext4) as uid 1000 — configure,
  build, test, commit all unprivileged there. (On the old /mnt/c tree,
  drvfs denied chmod/utime to non-root so cmake configure required root.)
  /tmp is cleaned across WSL reboots (persistent artifacts live outside
  /tmp); Write-tool files arrive root-owned (chown pass before building).

## Block 1 verification (all as uid 1000 in ~/src/snowglobe ext4 unless noted)
- Deps: cpp-httplib v0.20.1 via FetchContent SYSTEM + OpenSSL 3.0.13
  (dynamic link; static deferred to Phase 4).
- Unit: redact (header set, userinfo, Bearer/Basic, key shapes, query,
  env values, UUID/SHA survival) + proxy utils (base64, utf8, model/stream
  incl. nested-model trap) — 81 assertions green; full suite 26/26 debug,
  26/26 asan-ubsan, zero findings.
- Manual proxy vs python mock: POST forwarding intact (Authorization
  untouched upstream, REDACTED stored), model parsed, SSE 59 B over ~1 s
  with per-chunk .idx timing, /u/ + --upstream + 429/500/404 passthrough,
  disconnect → truncated:true + healthy proxy, TLS 200 with SSL_CERT_FILE
  CA / 502 with wrong CA (verification ON).
- Wire-proven framing: single Content-Type, zero-chunk terminator present
  (curl exit 18 root-caused to its absence in an earlier revision).
- TSan cannot execute on this WSL box at all (hello-world fails identically:
  unexpected memory mapping) — environmental; CI adjudicates in Block 2.

## Block 2 verification (all as uid 1000 in ~/src/snowglobe ext4 unless noted)
- Mock (`test/mockllm/server.py`) smoke: non-stream verbatim, SSE chunked +
  `[DONE]`, `/test-data`, verbatim `Authorization` in headers log, per-req
  req/res bodies in `--sent-dir`.
- Toy agent direct-vs-mock: exit 0, `done`, `note.txt` = `hello snowglobe`.
- Snowglobe e2e: `run --upstream=openai=<mock> -- agent.py` → exit 0,
  `3 LLM turns`, stored `Authorization: REDACTED`, mock saw 3× real
  `Bearer`, secret grep over run dir 0.
- `ctest --preset debug`: 34/34 (20.2 s, incl. 8 new proxy tests);
  `asan-ubsan`: 34/34 (26.1 s), zero findings. Latency: first byte 10 ms
  (<1.5 s), median TTFB delta 1.2 ms debug (budget 5 ms) / 6.0 ms asan
  (budget 25 ms, sanitizer-scaled — see test/CMakeLists).
- New tests caught a real bug: tracer `run.meta` cmd unredacted (fixed in
  `ptrace_tracer.cpp`; goldens 8/8 still green, no golden change needed).
- `tsan` preset compiles + links clean; cannot execute on WSL
  (memory-mapping, incl. Catch discovery). CI 22.04-tsan caught one real
  finding: concurrent first-use compile of httplib's thread_local
  response-line regex races in libstdc++-11 internals (full stacks in
  CHANGELOG; our code has zero regex/locale use, 24.04 clean) —
  `test/tsan.supp` suppresses exactly those three frames for proxy tests
  in tsan builds only; all other tests keep full TSan coverage.
- Bench regen (release, WSL2): strace column + median event counts;
  forkexec 12.65× (2116 ev), python-import 4.43× (137 ev), git-status
  12.86× (43 ev), find-usrlib 2.73× (24 ev). CI file regenerates on push.

## Block 3 verification (all as uid 1000 in ~/src/snowglobe ext4 unless noted)
- No key, no `claude`, zero key-like env on the box → toy-agent fallback
  fixture `test/fixtures/real/toy-agent-3turn/` (125 normalised events, 9
  blobs, secret + tmpdir grep 0), explicitly labelled; real-recording
  issue stays open.
- Fixture epilogue: `exit: 0 | events: 125 | 3 LLM turns`.
- D1 (builder==reviewer) over `core/proxy/` + `core/redact/`: 1 High
  (OpenAI bases lacked `/v1` — fixed + proven live), 1 Medium
  (`this`-capture in detached thread — fixed), 4 Low/Info documented, no
  fix. Full table in the session summary.
- Post-D1: `ctest --preset debug` 34/34, `asan-ubsan` 34/34, zero
  findings; `clang-format --dry-run --Werror` clean (proxy reindent
  accepted from the tool).

## Phase 1C verification (all as uid 1000 in ~/src/snowglobe ext4 unless noted)
- App: `npm ci` (75 pkgs, lockfile committed) + `next build` clean, 80 KB
  first-load JS, 436 KB `out/`; output contains no runtime external refs
  (only inert framework strings).
- `vitest`: 8/8 (SSE framing/deltas, usage, cost, toy-agent blobs).
- `snowglobe view`: embedded page + manifest/events/blobs served, 404s +
  traversal guard + bad-run 64 verified by hand.
- Playwright (chromium, local): toy-agent e2e (3 turns, turn-2 file-write
  linkage, timeline/files/network tabs, zero non-localhost requests) and
  50 MB perf e2e green.
- Full suites include the new `viewer_unit`/`viewer_e2e` (e2e runs for
  real where browsers exist, SKIP-prints otherwise): debug 36/36 + asan
  36/36 as uid 1000, zero findings; cross-aarch64 11/11 with
  `SNOWGLOBE_VIEWER=OFF`.

## Block 3 verification (all as uid 1000 in ~/src/snowglobe ext4 unless noted)
- tcp_loopback (23 events): binds + blocking connects initiated:true;
  non-blocking to 192.0.2.1:80 initiated:true/ok:false (EINPROGRESS proven);
  closed-port connect initiated:false/ok:false; deterministic across reruns.
- Bench: forkexec-300 13.79×, python-import 4.38×, git-status 11.75×,
  find-usrlib 2.63× (release, WSL2, medians of 5). CI bench job commits
  its own md separately.
- Realistic fixture: `pip download requests`, 1025 normalised events,
  0 decode_error, key-material grep 0. No API keys on the box (WSL, tester,
  Windows all checked); "record real-agent fixtures" issue left for human.
- D1 self-review of core/tracer/ptrace (builder==reviewer here; re-run with
  a second model per Part E): 2 High (argv truncation → truncated:true;
  first-stop death → loud error) + 6 Medium fixed, verified (nested tracer
  errors LOUD with message; 100-arg exec flagged truncated:true).
- ctest debug + asan-ubsan 15/15 (incl. tcp_loopback), zero sanitizer
  findings; clang-format clean.

## Next up (ordered)
1. Phase 1A — tracer hardening (6 golden trees, kill-9 no-orphans, bench publish)
2. Phase 1B — LLM proxy + mock + toy agent
3. Phase 1C — embedded viewer

## Known issues / tech debt (with issue links)
- #7 `--capture-stdio` passthrough only (logs created empty) — Phase 1A
- `net.connect` addr formatting only (family/addr/port split deferred) — Phase 1A
- CLI11/nlohmann/json not yet vendored (hand-rolled JSON; proxy phase adds them) — Phase 1B
- No `poc/tracer.c` found in workspace; tracer written fresh from spec — recorded here

## Capability matrix on the dev box (from `snowglobe doctor`, WSL2 Ubuntu 24.04)
| userns | overlayfs-in-userns | landlock | seccomp-notif | cgroup v2 delegated | pasta |
| ✅ (apparmor knob absent) | ✅ (mounted+verified) | ✅ abi 3 | ✅ | ✅ | ❌ |

## Decisions pending the human
- Create the GitHub repo + push (no remote/auth on this box) — push guide sent;
  paste the Actions run URL; CI green is the last gate before DONE

## Metrics (weekly)
stars · installs · WAU CLIs · runs · shares · interviews done · MRR — all zero (pre-alpha)
# Changelog

All notable changes to this project will be documented in this file.
Format: Keep a Changelog. Versioning: SemVer (schema v0 until v0.1.0).

## [Unreleased]

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

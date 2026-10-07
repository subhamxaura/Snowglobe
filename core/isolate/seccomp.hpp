#pragma once
// Seccomp allowlist-inverse for --isolate (ADR-0007): default ALLOW,
// ERRNO(EPERM) on a Docker-parity blocklist, ENOSYS on clone3 (glibc
// falls back to clone), KILL_PROCESS on io_uring_setup (strict superset,
// fail LOUD never silent). Arg-filtered: clone/unshare deny namespace
// creation, socket denies AF_ALG/AF_VSOCK, personality allows only the
// Docker-permitted values. Raw seccomp(2) + classic BPF on purpose: no
// libseccomp headers exist here (and none are needed for the static
// binary). Installed in the middle after all mounts, inherited by init +
// agent (never the tracer).
#include <cstdint>
#include <string>
#include <vector>

#ifdef __linux__
#include <linux/filter.h>
#endif

namespace snowglobe::isolate {

// One blocked call: ERRNO(err) by default EPERM, except io_uring_setup
// which kills loudly (fail LOUD, never silent io_uring invisibility)
// and clone3 which returns ENOSYS so glibc falls back to clone.
struct BlockedCall {
  int nr = 0;
  const char* name = nullptr;
  bool kill = false;
  int err = 1; // EPERM default; 38 (ENOSYS) for clone3
};

// Blocklist with per-arch numbers (SYS_* guarded; missing = skipped).
// Exported for tests + doctor probes. Mirrors Docker default profile
// (moby/profiles seccomp/default.json) for unprivileged containers:
// every unconditionally-blocked call plus the CAP-gated calls we never
// grant (modules, rawio, time, NUMA, keyring, etc.). clone/unshare/socket/
// personality are NOT in this list (arg-filtered in the BPF builder).
// ptrace/process_vm/kcmp stay blocked as a strict superset (Docker allows
// them on kernel >= 4.8 / with CAP_SYS_PTRACE; we deny nested tracing and
// host inspection). io_uring_setup KILL is a strict superset (Docker ERRNO).
std::vector<BlockedCall> isolateBlocklist();

// Deterministic BPF program for the blocklist (empty = unsupported arch).
// Layout: arch check (fail closed, KILL on mismatch) + one JEQ per simple
// call + arg-filtered checks (clone/unshare/socket/personality) + ALLOW +
// ERRNO(EPERM) + ERRNO(ENOSYS) + KILL. The LD-nr thread is fail-closed:
// any arch mismatch kills, never allows.
#ifdef __linux__
std::vector<struct sock_filter> buildIsolateFilter(const std::vector<BlockedCall>& calls);
// Arch-override builder for tests: proves the non-native arch path kills
// (fail-open class). Production calls this with the native arch.
std::vector<struct sock_filter> buildIsolateFilterWithArch(const std::vector<BlockedCall>& calls,
                                                           unsigned int auditArch);
#endif

// Install in the calling process (needs NO_NEW_PRIVS first, done here).
// False + error → caller exits 69. One-way: children inherit, no escape.
bool installIsolateFilter(std::string& error);

} // namespace snowglobe::isolate

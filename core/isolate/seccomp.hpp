#pragma once
// Seccomp allowlist-inverse for --isolate (ADR-0007): default ALLOW,
// ERRNO(EPERM) on a curated blocklist, KILL_PROCESS on io_uring_setup.
// Raw seccomp(2) + classic BPF on purpose: no libseccomp headers exist
// here (and none are needed for the static binary). Installed in the
// middle after all mounts, inherited by init + agent (never the tracer).
#include <cstdint>
#include <string>
#include <vector>

#ifdef __linux__
#include <linux/filter.h>
#endif

namespace snowglobe::isolate {

// One blocked call: EPERM, except io_uring_setup which kills loudly
// (fail LOUD, never silent io_uring invisibility).
struct BlockedCall {
  int nr = 0;
  const char* name = nullptr;
  bool kill = false;
};

// Blocklist with per-arch numbers (SYS_* guarded; missing = skipped).
// Exported for tests + doctor probes.
std::vector<BlockedCall> isolateBlocklist();

// Deterministic BPF program for the blocklist (empty = unsupported arch).
// Layout: arch check (fail closed) + one JEQ per call + ALLOW + actions.
#ifdef __linux__
std::vector<struct sock_filter> buildIsolateFilter(const std::vector<BlockedCall>& calls);
#endif

// Install in the calling process (needs NO_NEW_PRIVS first, done here).
// False + error → caller exits 69. One-way: children inherit, no escape.
bool installIsolateFilter(std::string& error);

} // namespace snowglobe::isolate

#pragma once
// seccomp user-notification filter for the notify backend (ADR-0009).
// Default ALLOW; SECCOMP_RET_USER_NOTIF on exactly the syscalls the
// tracer decodes (the same set the ptrace backend classifies — parity by
// construction for the trapped set). Raw seccomp(2) + classic BPF like
// the isolate filter (no libseccomp; static binary stays dependency-free).
//
// arch mismatch degrades to ALLOW (unlike the isolate filter's KILL):
// this is a visibility tool, not a sandbox — an unobservable 32-bit
// child keeps running with lifecycle tracing rather than dying.
// x32 numbers (R5 class) trap too: the supervisor strips the high bit.
// Thread ownership: pure builders, safe from any thread.
#include <cstdint>
#include <string>
#include <vector>

#ifdef __linux__
#include <linux/filter.h>
#endif

namespace snowglobe::tracer {

// Observed syscalls, arch-native numbers ({name, nr}); the trapped set.
// Mirrors the ptrace Kind classification exactly: execve/execveat, open
// family, unlink family, rename family, mkdir family, connect/sendto/
// bind, symlink family, chmod family.
struct ObservedCall {
  int nr = 0;
  const char* name = nullptr;
};

std::vector<ObservedCall> notifyObserved();

#ifdef __linux__
// Deterministic BPF program (empty = unsupported arch): arch check
// (mismatch -> ALLOW) + x32-range trap + one JEQ per observed call +
// ALLOW + USER_NOTIF. The LD-nr thread is fail-open by policy here
// (arch mismatch allows — visibility degrades, children never die;
// the isolate filter kills instead, different tool, different rule).
std::vector<struct sock_filter> buildNotifyFilter();
std::vector<struct sock_filter> buildNotifyFilterWithArch(unsigned int auditArch);
#endif

} // namespace snowglobe::tracer

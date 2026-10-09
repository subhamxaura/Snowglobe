// seccomp user-notification backend — Block 1 skeleton (ADR-0009).
// Thread ownership: single supervisor thread drives run(); no internal threads.
//
// What is REAL here: the capability probe (GET_ACTION_AVAIL), the
// honest EX_UNAVAILABLE paths, and the documented install invariant
// (single-threaded pre-fork, R6 — shared with the isolate installer).
// What is NOT here yet: the notification-fd loop and the USER_NOTIF
// filter builder (BLOCK 2, with parity + kill + bench proofs). run()
// therefore never installs a filter: a USER_NOTIF filter without a loop
// to answer it would wedge tracees in the kernel, so the skeleton fails
// LOUD instead of half-tracing.
#include "notify_tracer.hpp"

#include <string>
#include <utility>
#include <vector>

#ifndef __linux__
namespace snowglobe::tracer {
NotifyTracer* NotifyTracer::create() {
  return nullptr;
}
bool NotifyTracer::probeAvailable(std::string& reason) {
  reason = "seccomp user-notify requires Linux";
  return false;
}
int NotifyTracer::run(const std::vector<std::string>&, const TraceOptions&) {
  error_ = "notify backend requires Linux";
  return -kExitUnavailable;
}
} // namespace snowglobe::tracer
#else

#include <cerrno>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SECCOMP_GET_ACTION_AVAIL
#define SECCOMP_GET_ACTION_AVAIL 2
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SYS_seccomp
#if defined(__x86_64__)
#define SYS_seccomp 317
#elif defined(__aarch64__)
#define SYS_seccomp 277
#endif
#endif

namespace snowglobe::tracer {

NotifyTracer* NotifyTracer::create() {
  return new NotifyTracer();
}

bool NotifyTracer::probeAvailable(std::string& reason) {
#ifdef SYS_seccomp
  unsigned int act = SECCOMP_RET_USER_NOTIF;
  errno = 0;
  const long r = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
  if (r == 0) {
    reason.clear();
    return true;
  }
  if (errno == EINVAL) {
    reason = "SECCOMP_RET_USER_NOTIF unknown (kernel < 5.11)";
  } else if (errno == ENOSYS) {
    reason = "no seccomp(2)";
  } else {
    reason = std::strerror(errno);
  }
  return false;
#else
  reason = "no SYS_seccomp for this arch";
  return false;
#endif
}

int NotifyTracer::run(const std::vector<std::string>&, const TraceOptions&) {
  std::string reason;
  if (!probeAvailable(reason)) {
    error_ = "notify backend unavailable: " + reason;
    return -kExitUnavailable;
  }
  // Capability present but the notification loop lands in BLOCK 2
  // (parity + no-stranded-tracees kill test + bench). Installing a
  // USER_NOTIF filter without the answer loop would leave tracees
  // blocked in the kernel on supervisor death — fail LOUD instead.
  error_ = "notify backend skeleton (ADR-0009 Block 1): notification loop lands in "
           "Block 2; use --backend=ptrace";
  return -kExitUnavailable;
}

} // namespace snowglobe::tracer
#endif

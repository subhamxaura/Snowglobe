#pragma once
#include "../common/itracer.hpp"

#include <string>
#include <utility>
#include <vector>

namespace snowglobe::tracer {

// seccomp user-notification backend (ADR-0009, Phase 4 Block 2).
// Linux-only; on other platforms `create()` returns nullptr.
//
// Notification discipline (implemented in notify_tracer.cpp):
//   - lifecycle via PTRACE_SEIZE + PTRACE_O_TRACE{FORK,VFORK,CLONE,EXEC}
//     + PTRACE_O_EXITKILL (no PTRACE_SYSCALL stops, ever);
//   - observed syscalls delivered via SECCOMP_RET_USER_NOTIF (filter in
//     notify_filter.cpp), everything else SECCOMP_RET_ALLOW, zero stops;
//   - the supervisor polls the listener fd alongside waitpid and answers
//     every valid notification exactly once with
//     SECCOMP_USER_NOTIF_FLAG_CONTINUE — promptly, never holding a
//     blocked syscall hostage (decode failures emit trace.decode_error
//     first, then answer);
//   - SECCOMP_IOCTL_NOTIF_ID_VALID before every read; path args via
//     process_vm_readv through the shared trace_helpers (same reads as
//     the ptrace backend);
//   - supervisor death kills tracees via PTRACE_O_EXITKILL even
//     mid-notification — proven by the notify kill tests, not assumed.
// Semantic delta (honest, additive, schema stays 0): entry-only
// observation means no syscall-exit values, so outcome events carry
// result_known:false instead of ok/errno/fd; run.meta gains backend.
// A failed execve is indistinguishable at entry, so it surfaces as
// proc.exec with result_known:false — proc.exec_failed never appears
// under notify (pinned by goldens-notify).
// Filter install runs single-threaded pre-exec (R6: no TSYNC needed;
// going multithreaded before install without TSYNC is a bug).
// --isolate + notify is rejected (69): the isolate middle needs its own
// listener hand-over first (ADR-0009 follow-ups).
class NotifyTracer : public ITracer {
public:
  static NotifyTracer* create();
  int run(const std::vector<std::string>& argv, const TraceOptions& opts) override;
  const std::string& error() const override {
    return error_;
  }

  // Capability probe (honest yes/no): SECCOMP_GET_ACTION_AVAIL for
  // SECCOMP_RET_USER_NOTIF (kernel >= 5.11). False + reason when missing.
  static bool probeAvailable(std::string& reason);

private:
  std::string error_;
};
} // namespace snowglobe::tracer

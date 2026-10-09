#pragma once
#include "../common/itracer.hpp"

#include <string>
#include <utility>
#include <vector>

namespace snowglobe::tracer {

// seccomp user-notification backend (ADR-0009, Phase 4 Block 1 skeleton).
// Linux-only; on other platforms `create()` returns nullptr.
//
// Specified notification discipline (BLOCK 2 implements the loop; this
// skeleton only probes capability and fails LOUD — it never installs a
// USER_NOTIF filter without a loop to answer it, so no tracee can be
// wedged by this code):
//   - lifecycle via PTRACE_SEIZE + PTRACE_O_TRACE{FORK,VFORK,CLONE,EXEC}
//     + PTRACE_O_EXITKILL (no PTRACE_SYSCALL stops);
//   - observed syscalls delivered via SECCOMP_RET_USER_NOTIF, everything
//     else SECCOMP_RET_ALLOW with zero stops;
//   - poll the notification fd alongside the ptrace wait loop;
//     SECCOMP_IOCTL_NOTIF_ID_VALID before every read; answer every
//     notification promptly with SECCOMP_USER_NOTIF_FLAG_CONTINUE (never
//     hold a blocking syscall hostage); path args via process_vm_readv
//     exactly as the ptrace backend reads them;
//   - supervisor death must not strand tracees: BLOCK 2 ships a kill
//     test proving it (supervisor SIGKILL mid-run, no survivors).
// Semantic delta (honest, additive, schema stays 0): entry-only
// observation means no syscall-exit values, so outcome events carry
// result_known:false instead of ok/errno/fd; run.meta gains backend.
// Filter install runs single-threaded pre-exec/pre-fork (R6: no TSYNC
// needed; going multithreaded before install without TSYNC is a bug).
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

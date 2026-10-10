// seccomp user-notification filter builder (see notify_filter.hpp).
#include "notify_filter.hpp"

#ifdef __linux__
#include <cassert>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <sys/syscall.h>

#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif

namespace snowglobe::tracer {
namespace {

#if defined(__x86_64__)
constexpr unsigned int kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr unsigned int kAuditArch = AUDIT_ARCH_AARCH64;
#else
constexpr unsigned int kAuditArch = 0; // unsupported: builder yields empty
#endif

// x32 ABI bit (R5 class): x32 syscalls reuse AUDIT_ARCH_X86_64 with this
// bit set in nr, so every nr-JEQ below would miss them. Trap the whole
// high range into USER_NOTIF (never ALLOW unseen); the supervisor strips
// the bit before classifying, so x32 stays observed, never invisible.
constexpr unsigned int kX32Bit = 0x40000000U;

void pushObserved(std::vector<ObservedCall>& out, const char* name, int nr) {
  ObservedCall c;
  c.nr = nr;
  c.name = name;
  out.push_back(c);
}

} // namespace

std::vector<ObservedCall> notifyObserved() {
  std::vector<ObservedCall> out;
#ifdef SYS_execve
  pushObserved(out, "execve", SYS_execve);
#endif
#ifdef SYS_execveat
  pushObserved(out, "execveat", SYS_execveat);
#endif
#ifdef SYS_open
  pushObserved(out, "open", SYS_open);
#endif
#ifdef SYS_openat
  pushObserved(out, "openat", SYS_openat);
#endif
#ifdef SYS_openat2
  pushObserved(out, "openat2", SYS_openat2);
#endif
#ifdef SYS_creat
  pushObserved(out, "creat", SYS_creat);
#endif
#ifdef SYS_unlink
  pushObserved(out, "unlink", SYS_unlink);
#endif
#ifdef SYS_unlinkat
  pushObserved(out, "unlinkat", SYS_unlinkat);
#endif
#ifdef SYS_rmdir
  pushObserved(out, "rmdir", SYS_rmdir);
#endif
#ifdef SYS_rename
  pushObserved(out, "rename", SYS_rename);
#endif
#ifdef SYS_renameat
  pushObserved(out, "renameat", SYS_renameat);
#endif
#ifdef SYS_renameat2
  pushObserved(out, "renameat2", SYS_renameat2);
#endif
#ifdef SYS_mkdir
  pushObserved(out, "mkdir", SYS_mkdir);
#endif
#ifdef SYS_mkdirat
  pushObserved(out, "mkdirat", SYS_mkdirat);
#endif
#ifdef SYS_connect
  pushObserved(out, "connect", SYS_connect);
#endif
#ifdef SYS_sendto
  pushObserved(out, "sendto", SYS_sendto);
#endif
#ifdef SYS_bind
  pushObserved(out, "bind", SYS_bind);
#endif
#ifdef SYS_symlink
  pushObserved(out, "symlink", SYS_symlink);
#endif
#ifdef SYS_symlinkat
  pushObserved(out, "symlinkat", SYS_symlinkat);
#endif
#ifdef SYS_chmod
  pushObserved(out, "chmod", SYS_chmod);
#endif
#ifdef SYS_fchmod
  pushObserved(out, "fchmod", SYS_fchmod);
#endif
#ifdef SYS_fchmodat
  pushObserved(out, "fchmodat", SYS_fchmodat);
#endif
  return out;
}

std::vector<struct sock_filter> buildNotifyFilterWithArch(unsigned int auditArch) {
  const std::vector<ObservedCall> calls = notifyObserved();
  std::vector<struct sock_filter> prog;
  if (auditArch == 0 || calls.empty()) {
    return prog; // unsupported arch (caller exits 69) or nothing to trap
  }
  auto loadAbs = [](unsigned int off) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_LD + BPF_W + BPF_ABS);
    ins.k = off;
    return ins;
  };
  auto jumpEq = [](unsigned int k, unsigned char jt, unsigned char jf) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_JMP + BPF_JEQ + BPF_K);
    ins.k = k;
    ins.jt = jt;
    ins.jf = jf;
    return ins;
  };
  auto jumpGe = [](unsigned int k, unsigned char jt, unsigned char jf) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_JMP + BPF_JGE + BPF_K);
    ins.k = k;
    ins.jt = jt;
    ins.jf = jf;
    return ins;
  };
  // Forward jump distance with a load-bearing range check (R4): BPF jt/jf
  // are 8-bit, so a target farther than 255 would silently truncate into a
  // wrong program. This filter is small (~30 insns); this trips loudly in
  // debug builds the day growth threatens it.
  auto fwdOff = [](size_t target, size_t cur) -> unsigned char {
    assert(target > cur);
    assert(target - cur - 1 <= 255);
    return static_cast<unsigned char>(target - cur - 1);
  };
  auto ret = [](unsigned int k) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_RET + BPF_K);
    ins.k = k;
    return ins;
  };

  // [0] LD arch, [1] JEQ arch (mismatch -> ALLOWIdx), [2] LD nr,
  // [3] JGE x32bit -> NOTIF, [4..] JEQ per observed call -> NOTIF,
  // then ALLOW + USER_NOTIF.
  const size_t callBase = 4;
  const size_t allowIdx = callBase + calls.size();
  const size_t notifIdx = allowIdx + 1;

  prog.push_back(loadAbs(4)); // offsetof(struct seccomp_data, arch)
  // Arch mismatch degrades to ALLOW (visibility tool, not sandbox: an
  // unobservable child keeps running with lifecycle tracing; the
  // isolate filter kills instead — different tool, different rule).
  prog.push_back(jumpEq(auditArch, 0, fwdOff(allowIdx, 1)));
  prog.push_back(loadAbs(0)); // offsetof(struct seccomp_data, nr)
  prog.push_back(jumpGe(kX32Bit, fwdOff(notifIdx, 3), 0));
  for (size_t i = 0; i < calls.size(); ++i) {
    prog.push_back(
        jumpEq(static_cast<unsigned int>(calls[i].nr), fwdOff(notifIdx, callBase + i), 0));
  }
  prog.push_back(ret(SECCOMP_RET_ALLOW));
  prog.push_back(ret(SECCOMP_RET_USER_NOTIF));
  return prog;
}

std::vector<struct sock_filter> buildNotifyFilter() {
  return buildNotifyFilterWithArch(kAuditArch);
}

} // namespace snowglobe::tracer
#else

namespace snowglobe::tracer {
std::vector<ObservedCall> notifyObserved() {
  return {};
}
} // namespace snowglobe::tracer
#endif

// Seccomp allowlist-inverse for --isolate (ADR-0007). Raw BPF, no
// libseccomp: headers are absent here and the static binary stays
// dependency-free. Fail closed throughout (arch mismatch kills).
#include "seccomp.hpp"

#ifdef __linux__
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif
#ifndef SECCOMP_RET_ERRNO
#define SECCOMP_RET_ERRNO 0x00050000U
#endif

namespace snowglobe::isolate {
namespace {

#if defined(__x86_64__)
constexpr unsigned int kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr unsigned int kAuditArch = AUDIT_ARCH_AARCH64;
#else
constexpr unsigned int kAuditArch = 0; // unsupported: builder yields empty
#endif

void pushErrno(std::vector<BlockedCall>& out, const char* name, int nr) {
  out.push_back(BlockedCall{nr, name, false});
}

} // namespace

std::vector<BlockedCall> isolateBlocklist() {
  std::vector<BlockedCall> out;
#ifdef SYS_mount
  pushErrno(out, "mount", SYS_mount);
#endif
#ifdef SYS_umount2
  pushErrno(out, "umount", SYS_umount2);
#endif
#ifdef SYS_pivot_root
  pushErrno(out, "pivot_root", SYS_pivot_root);
#endif
#ifdef SYS_kexec_load
  pushErrno(out, "kexec_load", SYS_kexec_load);
#endif
#ifdef SYS_kexec_file_load
  pushErrno(out, "kexec_file_load", SYS_kexec_file_load);
#endif
#ifdef SYS_reboot
  pushErrno(out, "reboot", SYS_reboot);
#endif
#ifdef SYS_init_module
  pushErrno(out, "init_module", SYS_init_module);
#endif
#ifdef SYS_finit_module
  pushErrno(out, "finit_module", SYS_finit_module);
#endif
#ifdef SYS_ptrace
  pushErrno(out, "ptrace", SYS_ptrace);
#endif
#ifdef SYS_bpf
  pushErrno(out, "bpf", SYS_bpf);
#endif
#ifdef SYS_perf_event_open
  pushErrno(out, "perf_event_open", SYS_perf_event_open);
#endif
#ifdef SYS_userfaultfd
  pushErrno(out, "userfaultfd", SYS_userfaultfd);
#endif
#ifdef SYS_swapon
  pushErrno(out, "swapon", SYS_swapon);
#endif
#ifdef SYS_acct
  pushErrno(out, "acct", SYS_acct);
#endif
#ifdef SYS_open_by_handle_at
  pushErrno(out, "open_by_handle_at", SYS_open_by_handle_at);
#endif
#ifdef SYS_io_uring_setup
  out.push_back(BlockedCall{SYS_io_uring_setup, "io_uring_setup", true});
#endif
  return out;
}

std::vector<struct sock_filter> buildIsolateFilter(const std::vector<BlockedCall>& calls) {
  std::vector<struct sock_filter> prog;
  if (kAuditArch == 0 || calls.empty()) {
    return prog; // unsupported arch (caller exits 69) or nothing to block
  }
  auto loadAbs = [](unsigned int off) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_LD + BPF_W + BPF_ABS);
    ins.k = off;
    return ins;
  };
  const size_t archBase = 0; // [0] LD arch, [1] JEQ arch
  const size_t nrBase = 2;   // [2] LD nr
  const size_t callBase = 3; // [3..] one JEQ per call
  const size_t allowIdx = callBase + calls.size();
  const size_t errnoIdx = allowIdx + 1;
  const size_t killIdx = errnoIdx + 1;
  (void)archBase;
  auto jump = [](uint16_t jt, uint16_t jf) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_JMP + BPF_JEQ + BPF_K);
    ins.jt = static_cast<unsigned char>(jt);
    ins.jf = static_cast<unsigned char>(jf);
    return ins;
  };
  // [0] A = audit arch.
  prog.push_back(loadAbs(4)); // offsetof(struct seccomp_data, arch)
  // [1] arch match else KILL (fail closed: x32 and friends die loudly).
  {
    struct sock_filter ins = jump(0, 0);
    ins.k = kAuditArch;
    // jt=0 falls into the nr load; jf lands on KILL.
    ins.jf = static_cast<unsigned char>(killIdx - (nrBase - 1) - 1);
    prog.push_back(ins);
  }
  // [2] A = syscall number.
  prog.push_back(loadAbs(0)); // offsetof(struct seccomp_data, nr)
  // [3..] one JEQ per blocked call.
  for (size_t i = 0; i < calls.size(); ++i) {
    struct sock_filter ins = jump(0, 0);
    ins.k = static_cast<unsigned int>(calls[i].nr);
    const size_t target = calls[i].kill ? killIdx : errnoIdx;
    ins.jt = static_cast<unsigned char>(target - (callBase + i) - 1);
    prog.push_back(ins);
  }
  // ALLOW; then the two shared actions.
  struct sock_filter allow = {};
  allow.code = static_cast<unsigned short>(BPF_RET + BPF_K);
  allow.k = SECCOMP_RET_ALLOW;
  prog.push_back(allow);
  struct sock_filter errn = {};
  errn.code = static_cast<unsigned short>(BPF_RET + BPF_K);
  errn.k = SECCOMP_RET_ERRNO | static_cast<unsigned int>(EPERM);
  prog.push_back(errn);
  struct sock_filter kill = {};
  kill.code = static_cast<unsigned short>(BPF_RET + BPF_K);
  kill.k = SECCOMP_RET_KILL_PROCESS;
  prog.push_back(kill);
  return prog;
}

bool installIsolateFilter(std::string& error) {
  const std::vector<BlockedCall> calls = isolateBlocklist();
  const std::vector<struct sock_filter> prog = buildIsolateFilter(calls);
  if (prog.empty()) {
    error = "isolate: seccomp unsupported on this arch";
    return false;
  }
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    error = std::string("isolate: no_new_privs: ") + ::strerror(errno);
    return false;
  }
  struct sock_fprog fprog = {};
  fprog.len = static_cast<unsigned short>(prog.size());
  // seccomp() reads the program synchronously; the vector outlives it.
  fprog.filter = const_cast<struct sock_filter*>(prog.data());
  if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
    error = std::string("isolate: seccomp filter: ") + ::strerror(errno);
    return false;
  }
  return true;
}

} // namespace snowglobe::isolate
#else

namespace snowglobe::isolate {

std::vector<BlockedCall> isolateBlocklist() {
  return {};
}

bool installIsolateFilter(std::string& error) {
  error = "isolate: requires Linux";
  return false;
}

} // namespace snowglobe::isolate
#endif

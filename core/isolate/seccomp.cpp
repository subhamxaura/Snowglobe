// Seccomp allowlist-inverse for --isolate (ADR-0007). Raw BPF, no
// libseccomp: headers are absent here and the static binary stays
// dependency-free. Fail closed throughout (arch mismatch kills).
// Docker parity: moby/profiles seccomp/default.json (unprivileged).
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

// Namespace-creation mask for clone/unshare arg filtering: NEWNS|NEWCGROUP|
// NEWUTS|NEWIPC|NEWUSER|NEWPID|NEWNET|NEWTIME. Docker's mask 0x7E020000
// excludes NEWTIME (0x80, overlaps signal bits); we include it (0x7E020080)
// as a strict superset — time namespaces stay denied too.
constexpr unsigned int kNsMask = 0x7E020080U;
// Blocked socket families (Docker: AF_ALG + AF_VSOCK only; everything else
// stays allowed — networking works).
constexpr unsigned int kAfAlg = 38;
constexpr unsigned int kAfVsock = 40;

void pushErrno(std::vector<BlockedCall>& out, const char* name, int nr) {
  // Named local (not a braced temporary): GCC 13 -O3 misreads the
  // aggregate copy as a 1-byte overflow (false positive, aarch64 CI).
  BlockedCall c;
  c.nr = nr;
  c.name = name;
  c.kill = false;
  c.err = EPERM;
  out.push_back(c);
}

void pushErrnoWith(std::vector<BlockedCall>& out, const char* name, int nr, int err) {
  BlockedCall c;
  c.nr = nr;
  c.name = name;
  c.kill = false;
  c.err = err;
  out.push_back(c);
}

} // namespace

std::vector<BlockedCall> isolateBlocklist() {
  std::vector<BlockedCall> out;
  // --- Pre-existing (Block 2) ---
#ifdef SYS_mount
  pushErrno(out, "mount", SYS_mount);
#endif
#ifdef SYS_umount2
  pushErrno(out, "umount2", SYS_umount2);
#endif
#ifdef SYS_umount
  pushErrno(out, "umount", SYS_umount);
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
  // --- Docker parity additions (Block 3): modules/obsolete ---
#ifdef SYS_create_module
  pushErrno(out, "create_module", SYS_create_module);
#endif
#ifdef SYS_delete_module
  pushErrno(out, "delete_module", SYS_delete_module);
#endif
#ifdef SYS_query_module
  pushErrno(out, "query_module", SYS_query_module);
#endif
#ifdef SYS_get_kernel_syms
  pushErrno(out, "get_kernel_syms", SYS_get_kernel_syms);
#endif
#ifdef SYS_sysfs
  pushErrno(out, "sysfs", SYS_sysfs);
#endif
#ifdef SYS__sysctl
  pushErrno(out, "_sysctl", SYS__sysctl);
#endif
#ifdef SYS_sysctl
  pushErrno(out, "sysctl", SYS_sysctl);
#endif
#ifdef SYS_uselib
  pushErrno(out, "uselib", SYS_uselib);
#endif
#ifdef SYS_ustat
  pushErrno(out, "ustat", SYS_ustat);
#endif
#ifdef SYS_nfsservctl
  pushErrno(out, "nfsservctl", SYS_nfsservctl);
#endif
#ifdef SYS_vm86
  pushErrno(out, "vm86", SYS_vm86);
#endif
#ifdef SYS_vm86old
  pushErrno(out, "vm86old", SYS_vm86old);
#endif
  // --- rawio ---
#ifdef SYS_ioperm
  pushErrno(out, "ioperm", SYS_ioperm);
#endif
#ifdef SYS_iopl
  pushErrno(out, "iopl", SYS_iopl);
#endif
  // --- io_uring (setup stays KILL as strict superset; enter/register ERRNO) ---
#ifdef SYS_io_uring_enter
  pushErrno(out, "io_uring_enter", SYS_io_uring_enter);
#endif
#ifdef SYS_io_uring_register
  pushErrno(out, "io_uring_register", SYS_io_uring_register);
#endif
#ifdef SYS_io_uring_setup
  out.push_back(BlockedCall{SYS_io_uring_setup, "io_uring_setup", true, EPERM});
#endif
  // --- keyring (not namespaced, always blocked) ---
#ifdef SYS_add_key
  pushErrno(out, "add_key", SYS_add_key);
#endif
#ifdef SYS_keyctl
  pushErrno(out, "keyctl", SYS_keyctl);
#endif
#ifdef SYS_request_key
  pushErrno(out, "request_key", SYS_request_key);
#endif
  // --- time (not namespaced) ---
#ifdef SYS_clock_settime
  pushErrno(out, "clock_settime", SYS_clock_settime);
#endif
#ifdef SYS_clock_settime64
  pushErrno(out, "clock_settime64", SYS_clock_settime64);
#endif
#ifdef SYS_settimeofday
  pushErrno(out, "settimeofday", SYS_settimeofday);
#endif
#ifdef SYS_stime
  pushErrno(out, "stime", SYS_stime);
#endif
#ifdef SYS_clock_adjtime
  // Docker ALLOWS clock_adjtime (in the big allow list) but blocks
  // clock_settime; we mirror that exactly (adjtime stays allowed).
#endif
  // --- tracing/inspection (strict superset: Docker allows with PTRACE or
  // kernel >= 4.8; we deny nested tracing and host inspection) ---
#ifdef SYS_kcmp
  pushErrno(out, "kcmp", SYS_kcmp);
#endif
#ifdef SYS_process_vm_readv
  pushErrno(out, "process_vm_readv", SYS_process_vm_readv);
#endif
#ifdef SYS_process_vm_writev
  pushErrno(out, "process_vm_writev", SYS_process_vm_writev);
#endif
#ifdef SYS_lookup_dcookie
  pushErrno(out, "lookup_dcookie", SYS_lookup_dcookie);
#endif
#ifdef SYS_pidfd_getfd
  pushErrno(out, "pidfd_getfd", SYS_pidfd_getfd);
#endif
  // --- NUMA (Docker allows with NICE; we deny as strict superset — agents
  // must not rebind host memory policy) ---
#ifdef SYS_get_mempolicy
  pushErrno(out, "get_mempolicy", SYS_get_mempolicy);
#endif
#ifdef SYS_mbind
  pushErrno(out, "mbind", SYS_mbind);
#endif
#ifdef SYS_move_pages
  pushErrno(out, "move_pages", SYS_move_pages);
#endif
#ifdef SYS_set_mempolicy
  pushErrno(out, "set_mempolicy", SYS_set_mempolicy);
#endif
#ifdef SYS_set_mempolicy_home_node
  pushErrno(out, "set_mempolicy_home_node", SYS_set_mempolicy_home_node);
#endif
  // --- quotas/swap ---
#ifdef SYS_quotactl
  pushErrno(out, "quotactl", SYS_quotactl);
#endif
#ifdef SYS_quotactl_fd
  pushErrno(out, "quotactl_fd", SYS_quotactl_fd);
#endif
#ifdef SYS_swapoff
  pushErrno(out, "swapoff", SYS_swapoff);
#endif
  // --- hostname/domain/chroot/syslog/tty (CAP-gated, never granted) ---
#ifdef SYS_chroot
  pushErrno(out, "chroot", SYS_chroot);
#endif
#ifdef SYS_setdomainname
  pushErrno(out, "setdomainname", SYS_setdomainname);
#endif
#ifdef SYS_sethostname
  pushErrno(out, "sethostname", SYS_sethostname);
#endif
#ifdef SYS_syslog
  pushErrno(out, "syslog", SYS_syslog);
#endif
#ifdef SYS_vhangup
  pushErrno(out, "vhangup", SYS_vhangup);
#endif
  // --- new mount API (must not bypass mount/pivot_root blocks) ---
#ifdef SYS_fsconfig
  pushErrno(out, "fsconfig", SYS_fsconfig);
#endif
#ifdef SYS_fsmount
  pushErrno(out, "fsmount", SYS_fsmount);
#endif
#ifdef SYS_fsopen
  pushErrno(out, "fsopen", SYS_fsopen);
#endif
#ifdef SYS_fspick
  pushErrno(out, "fspick", SYS_fspick);
#endif
#ifdef SYS_mount_setattr
  pushErrno(out, "mount_setattr", SYS_mount_setattr);
#endif
#ifdef SYS_move_mount
  pushErrno(out, "move_mount", SYS_move_mount);
#endif
#ifdef SYS_open_tree
  pushErrno(out, "open_tree", SYS_open_tree);
#endif
#ifdef SYS_fanotify_init
  pushErrno(out, "fanotify_init", SYS_fanotify_init);
#endif
  // --- namespace entry (no arg filtering needed: legitimate agents never
  // join other namespaces; clone/unshare with NS flags are arg-filtered
  // below so thread/process creation keeps working) ---
#ifdef SYS_setns
  pushErrno(out, "setns", SYS_setns);
#endif
#ifdef SYS_clone3
  // ENOSYS (not EPERM) so glibc falls back to clone(2) for thread
  // creation — Docker parity (errnoRet 38). Plain clone stays allowed
  // without NS flags (arg-filtered below).
  pushErrnoWith(out, "clone3", SYS_clone3, ENOSYS);
#endif
  return out;
}

std::vector<struct sock_filter> buildIsolateFilterWithArch(const std::vector<BlockedCall>& calls,
                                                           unsigned int auditArch) {
  std::vector<struct sock_filter> prog;
  if (auditArch == 0 || calls.empty()) {
    return prog; // unsupported arch (caller exits 69) or nothing to block
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
  auto ret = [](unsigned int k) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_RET + BPF_K);
    ins.k = k;
    return ins;
  };
  auto aluAnd = [](unsigned int k) -> struct sock_filter {
    struct sock_filter ins = {};
    ins.code = static_cast<unsigned short>(BPF_ALU + BPF_AND + BPF_K);
    ins.k = k;
    return ins;
  };

  // Optional arg-filtered syscalls (present on this arch only).
#ifdef SYS_clone
  const bool hasClone = true;
  const int nrClone = SYS_clone;
#else
  const bool hasClone = false;
  const int nrClone = 0;
#endif
#ifdef SYS_unshare
  const bool hasUnshare = true;
  const int nrUnshare = SYS_unshare;
#else
  const bool hasUnshare = false;
  const int nrUnshare = 0;
#endif
#ifdef SYS_socket
  const bool hasSocket = true;
  const int nrSocket = SYS_socket;
#else
  const bool hasSocket = false;
  const int nrSocket = 0;
#endif
#ifdef SYS_personality
  const bool hasPersonality = true;
  const int nrPersonality = SYS_personality;
#else
  const bool hasPersonality = false;
  const int nrPersonality = 0;
#endif
  (void)hasClone;
  (void)hasUnshare;
  (void)hasSocket;
  (void)hasPersonality;

  // Instruction counts for the arg-filtered sections (fixed layout so jump
  // offsets are computable before emission).
  // clone: JEQ nr, LD arg0, AND mask, JEQ 0  (4 insns)
  const size_t cloneInsns = hasClone ? 4 : 0;
  // unshare: same shape (4 insns)
  const size_t unshareInsns = hasUnshare ? 4 : 0;
  // socket: JEQ nr, LD arg0, JEQ ALG, JEQ VSOCK (4 insns)
  const size_t socketInsns = hasSocket ? 4 : 0;
  // personality: JEQ nr, LD arg0, 5x JEQ allowed (7 insns)
  const size_t personalityInsns = hasPersonality ? 7 : 0;

  const size_t callBase = 3; // [0] LD arch, [1] JEQ arch, [2] LD nr
  const size_t simpleEnd = callBase + calls.size();
  const size_t cloneBase = simpleEnd;
  const size_t unshareBase = cloneBase + cloneInsns;
  const size_t socketBase = unshareBase + unshareInsns;
  const size_t personalityBase = socketBase + socketInsns;
  const size_t allowIdx = personalityBase + personalityInsns;
  const size_t errnoIdx = allowIdx + 1;
  const size_t enosysIdx = errnoIdx + 1;
  const size_t killIdx = enosysIdx + 1;

  // [0] A = audit arch.
  prog.push_back(loadAbs(4)); // offsetof(struct seccomp_data, arch)
  // [1] arch match else KILL (fail closed: x32 and friends die loudly).
  {
    const auto jf = static_cast<unsigned char>(killIdx - 1 - 1);
    prog.push_back(jumpEq(auditArch, 0, jf));
  }
  // [2] A = syscall number.
  prog.push_back(loadAbs(0)); // offsetof(struct seccomp_data, nr)
  // [3..] one JEQ per simple blocked call.
  for (size_t i = 0; i < calls.size(); ++i) {
    size_t target;
    if (calls[i].kill) {
      target = killIdx;
    } else if (calls[i].err == ENOSYS) {
      target = enosysIdx;
    } else {
      target = errnoIdx;
    }
    const auto jt = static_cast<unsigned char>(target - (callBase + i) - 1);
    prog.push_back(jumpEq(static_cast<unsigned int>(calls[i].nr), jt, 0));
  }
  // --- clone: deny namespace creation, allow thread/process creation ---
  if (hasClone) {
    // JEQ clone_nr -> arg check, else skip the 3 arg insns.
    prog.push_back(jumpEq(static_cast<unsigned int>(nrClone), 0, 3));
    prog.push_back(loadAbs(16)); // args[0] low 32 (flags)
    prog.push_back(aluAnd(kNsMask));
    // (flags & mask) == 0 -> continue to next check (allow path);
    // else ERRNO(EPERM). jt skips to unshareBase, jf lands on ERRNO.
    {
      const size_t cur = cloneBase + 3;
      const auto jt = static_cast<unsigned char>(unshareBase - cur - 1);
      const auto jf = static_cast<unsigned char>(errnoIdx - cur - 1);
      prog.push_back(jumpEq(0, jt, jf));
    }
  }
  // --- unshare: same shape (flags in args[0]) ---
  if (hasUnshare) {
    prog.push_back(jumpEq(static_cast<unsigned int>(nrUnshare), 0, 3));
    prog.push_back(loadAbs(16));
    prog.push_back(aluAnd(kNsMask));
    {
      const size_t cur = unshareBase + 3;
      const auto jt = static_cast<unsigned char>(socketBase - cur - 1);
      const auto jf = static_cast<unsigned char>(errnoIdx - cur - 1);
      prog.push_back(jumpEq(0, jt, jf));
    }
  }
  // --- socket: deny AF_ALG + AF_VSOCK only, allow everything else ---
  if (hasSocket) {
    prog.push_back(jumpEq(static_cast<unsigned int>(nrSocket), 0, 3));
    prog.push_back(loadAbs(16)); // args[0] = family
    // family == ALG -> ERRNO; else fall through to the VSOCK check.
    {
      const size_t cur = socketBase + 2;
      const auto jf = static_cast<unsigned char>(0); // next insn (VSOCK check)
      const auto jt = static_cast<unsigned char>(errnoIdx - cur - 1);
      prog.push_back(jumpEq(kAfAlg, jt, jf));
    }
    {
      const size_t cur = socketBase + 3;
      const auto jt = static_cast<unsigned char>(errnoIdx - cur - 1);
      const auto jf = static_cast<unsigned char>(personalityBase - cur - 1);
      prog.push_back(jumpEq(kAfVsock, jt, jf));
    }
  }
  // --- personality: allow only Docker's 5 values, deny the rest ---
  if (hasPersonality) {
    prog.push_back(jumpEq(static_cast<unsigned int>(nrPersonality), 0, 6));
    prog.push_back(loadAbs(16)); // args[0] = persona
    // Each allowed value jumps forward to personalityBase end (next check /
    // ALLOW); the last fallthrough lands on ERRNO.
    const unsigned int allowed[] = {0, 8, 131072, 131080, 4294967295U};
    for (size_t i = 0; i < 5; ++i) {
      const size_t cur = personalityBase + 2 + i;
      const size_t nextCheck = personalityBase + personalityInsns; // == allowIdx
      if (i < 4) {
        // Allowed -> jump directly to ALLOW (skip remaining value checks);
        // otherwise fall through to the next value check.
        const auto jt = static_cast<unsigned char>(nextCheck - cur - 1);
        prog.push_back(jumpEq(allowed[i], jt, 0));
      } else {
        // Last value: jt -> allow path, jf -> ERRNO.
        const auto jt = static_cast<unsigned char>(nextCheck - cur - 1);
        const auto jf = static_cast<unsigned char>(errnoIdx - cur - 1);
        prog.push_back(jumpEq(allowed[i], jt, jf));
      }
    }
  }
  // ALLOW; then the three shared actions.
  prog.push_back(ret(SECCOMP_RET_ALLOW));
  prog.push_back(ret(SECCOMP_RET_ERRNO | static_cast<unsigned int>(EPERM)));
  prog.push_back(ret(SECCOMP_RET_ERRNO | static_cast<unsigned int>(ENOSYS)));
  struct sock_filter kill = {};
  kill.code = static_cast<unsigned short>(BPF_RET + BPF_K);
  kill.k = SECCOMP_RET_KILL_PROCESS;
  prog.push_back(kill);
  return prog;
}

std::vector<struct sock_filter> buildIsolateFilter(const std::vector<BlockedCall>& calls) {
  return buildIsolateFilterWithArch(calls, kAuditArch);
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

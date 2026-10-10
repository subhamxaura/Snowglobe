// Phase 4 Block 2: notify-filter shape + a tiny cBPF emulator proving the
// verdict table (USER_NOTIF for the observed set incl. x32, ALLOW for
// everything else incl. arch mismatch). Pure computation, no kernel
// needed; the live notification flow is proven by goldens-notify/.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "tracer/notify/notify_filter.hpp"

#ifdef __linux__
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/syscall.h>
#endif

namespace {

#ifdef __linux__
// Minimal cBPF interpreter for exactly the subset the notify builder
// emits (LD_ABS W, JEQ K, JGE K, RET K). Returns the RET k value.
unsigned int runBpf(const std::vector<struct sock_filter>& prog, unsigned int arch,
                    unsigned int nr) {
  // Fake seccomp_data image: nr at 0, arch at 4.
  size_t pc = 0;
  unsigned int acc = 0;
  size_t steps = 0;
  while (pc < prog.size() && steps < prog.size() + 16) {
    ++steps;
    const struct sock_filter& ins = prog[pc];
    const unsigned int cls = ins.code & 0x07;
    if (cls == (BPF_LD & 0x07) && ins.code == (BPF_LD + BPF_W + BPF_ABS)) {
      acc = (ins.k == 0) ? nr : (ins.k == 4) ? arch : 0;
      ++pc;
    } else if (cls == (BPF_JMP & 0x07)) {
      const unsigned int op = ins.code & 0xf0;
      bool take = false;
      if (op == (BPF_JEQ & 0xf0)) {
        take = (acc == ins.k);
      } else if (op == (BPF_JGE & 0xf0)) {
        take = (acc >= ins.k);
      } else {
        return 0xdead0000U; // unsupported op: loud tripwire
      }
      pc += take ? (size_t)ins.jt + 1 : (size_t)ins.jf + 1;
    } else if (cls == (BPF_RET & 0x07)) {
      return ins.k;
    } else {
      return 0xdead0001U; // unsupported class: loud tripwire
    }
  }
  return 0xdead0002U; // fell off: loud tripwire
}

unsigned int nativeArch() {
#if defined(__x86_64__)
  return AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
  return AUDIT_ARCH_AARCH64;
#else
  return 0;
#endif
}
#endif

} // namespace

TEST_CASE("notify observed set mirrors the ptrace decode set", "[notify]") {
  const auto calls = snowglobe::tracer::notifyObserved();
  std::set<std::string> names;
  for (const auto& c : calls) {
    names.insert(c.name != nullptr ? c.name : "");
  }
  // Exactly the ptrace Kind classification (exec/open/unlink/rename/mkdir/
  // connect/sendto/bind/symlink/chmod families incl. at-variants + creat).
  for (const char* want :
       {"execve", "execveat", "open",      "openat",    "openat2", "creat",   "unlink",  "unlinkat",
        "rmdir",  "rename",   "renameat",  "renameat2", "mkdir",   "mkdirat", "connect", "sendto",
        "bind",   "symlink",  "symlinkat", "chmod",     "fchmod",  "fchmodat"}) {
    CHECK(names.count(want) == 1);
  }
  CHECK(names.size() == 22);
}

#ifdef __linux__
TEST_CASE("notify filter verdict table", "[notify]") {
  const auto calls = snowglobe::tracer::notifyObserved();
  const auto prog = snowglobe::tracer::buildNotifyFilter();
  REQUIRE(!prog.empty());
  const unsigned int arch = nativeArch();
  REQUIRE(arch != 0);

  // Every observed nr traps (native + x32 high-bit form).
  for (const auto& c : calls) {
    const unsigned int nr = static_cast<unsigned int>(c.nr);
    CHECK(runBpf(prog, arch, nr) == SECCOMP_RET_USER_NOTIF);
    CHECK(runBpf(prog, arch, nr | 0x40000000U) == SECCOMP_RET_USER_NOTIF);
  }
  // Boring syscalls allow with zero stops (spot-check + a sweep).
  CHECK(runBpf(prog, arch, static_cast<unsigned int>(SYS_getpid)) == SECCOMP_RET_ALLOW);
  CHECK(runBpf(prog, arch, static_cast<unsigned int>(SYS_read)) == SECCOMP_RET_ALLOW);
  CHECK(runBpf(prog, arch, static_cast<unsigned int>(SYS_write)) == SECCOMP_RET_ALLOW);
  CHECK(runBpf(prog, arch, static_cast<unsigned int>(SYS_clone)) == SECCOMP_RET_ALLOW);
  for (unsigned int nr = 0; nr < 512; ++nr) {
    bool observed = false;
    for (const auto& c : calls) {
      if (static_cast<unsigned int>(c.nr) == nr) {
        observed = true;
      }
    }
    if (!observed) {
      CHECK(runBpf(prog, arch, nr) == SECCOMP_RET_ALLOW);
    }
  }
  // Arch mismatch degrades to ALLOW (visibility tool, not sandbox).
  CHECK(runBpf(prog, arch ^ 0x01000000U, static_cast<unsigned int>(SYS_openat)) ==
        SECCOMP_RET_ALLOW);
  CHECK(runBpf(prog, 0, static_cast<unsigned int>(SYS_openat)) == SECCOMP_RET_ALLOW);
}

TEST_CASE("notify filter arch override kills nothing, allows", "[notify]") {
  // The WithArch builder exists for tests: non-native arch programs must
  // still be well-formed (arch check first, ALLOW on mismatch).
  const auto prog = snowglobe::tracer::buildNotifyFilterWithArch(nativeArch() ^ 0x01000000U);
  REQUIRE(!prog.empty());
  CHECK(runBpf(prog, nativeArch(), static_cast<unsigned int>(SYS_openat)) == SECCOMP_RET_ALLOW);
  // Unsupported arch yields empty (caller exits 69).
  CHECK(snowglobe::tracer::buildNotifyFilterWithArch(0).empty());
}
#endif

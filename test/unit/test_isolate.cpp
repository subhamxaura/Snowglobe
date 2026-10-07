// Phase 2 Block 2: seccomp/landlock enforcement in forked children (the
// filters are one-way process state — never install them in the test
// process itself), plus pure parser/name checks.
#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/filter.h>
#endif

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

#include "isolate/isolate.hpp"
#include "isolate/landlock.hpp"
#include "isolate/seccomp.hpp"

TEST_CASE("isolate blocklist shape", "[isolate]") {
  const auto calls = snowglobe::isolate::isolateBlocklist();
  CHECK(!calls.empty());
  bool mountErrno = false;
  bool uringKill = false;
  bool ptraceErrno = false;
  bool addKey = false;
  bool keyctl = false;
  bool mbind = false;
  bool getMempolicy = false;
  bool uringEnter = false;
  bool uringRegister = false;
  bool clone3Enosys = false;
  bool setns = false;
  bool chroot = false;
  for (const auto& c : calls) {
    const std::string n = c.name != nullptr ? c.name : "";
    if (n == "mount" && !c.kill) {
      mountErrno = true;
    }
    if (n == "io_uring_setup" && c.kill) {
      uringKill = true;
    }
    if (n == "ptrace" && !c.kill) {
      ptraceErrno = true;
    }
    if (n == "add_key" && !c.kill && c.err == EPERM) {
      addKey = true;
    }
    if (n == "keyctl" && !c.kill) {
      keyctl = true;
    }
    if (n == "mbind" && !c.kill) {
      mbind = true;
    }
    if (n == "get_mempolicy" && !c.kill) {
      getMempolicy = true;
    }
    if (n == "io_uring_enter" && !c.kill) {
      uringEnter = true;
    }
    if (n == "io_uring_register" && !c.kill) {
      uringRegister = true;
    }
    if (n == "clone3" && !c.kill && c.err == ENOSYS) {
      clone3Enosys = true;
    }
    if (n == "setns" && !c.kill) {
      setns = true;
    }
    if (n == "chroot" && !c.kill) {
      chroot = true;
    }
  }
  CHECK(mountErrno);
  CHECK(uringKill);
  CHECK(ptraceErrno);
  // Docker-parity additions (Block 3): keyring, NUMA, io_uring enter/register,
  // clone3 ENOSYS (glibc fallback), setns, chroot.
  CHECK(addKey);
  CHECK(keyctl);
  CHECK(mbind);
  CHECK(getMempolicy);
  CHECK(uringEnter);
  CHECK(uringRegister);
  CHECK(clone3Enosys);
  CHECK(setns);
  CHECK(chroot);
  // clone/unshare/socket/personality are arg-filtered, never in the flat list.
  for (const auto& c : calls) {
    const std::string n = c.name != nullptr ? c.name : "";
    CHECK(n != "clone");
    CHECK(n != "unshare");
    CHECK(n != "socket");
    CHECK(n != "personality");
  }
  // Deterministic program, non-empty on supported arches.
  const auto prog = snowglobe::isolate::buildIsolateFilter(calls);
  CHECK(!prog.empty());
}

TEST_CASE("seccomp filter installs and spares live syscalls", "[isolate]") {
  const pid_t c = ::fork();
  REQUIRE(c >= 0);
  if (c == 0) {
    std::string err;
    if (!snowglobe::isolate::installIsolateFilter(err)) {
      _exit(10);
    }
    // Alive: getpid still works under the filter.
    if (::getpid() <= 0) {
      _exit(12);
    }
    _exit(0);
  }
  int st = 0;
  REQUIRE(::waitpid(c, &st, 0) == c);
  CHECK(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
  // NOTE: ERRNO-blocklist entries (mount, ptrace, ...) need privilege to
  // attempt, so their EPERM is proven under --isolate (mapped root has
  // caps; the call would otherwise succeed) in isolate_basic.py, not here.
}

TEST_CASE("seccomp filter kills io_uring_setup with SIGSYS", "[isolate]") {
#ifdef SYS_io_uring_setup
  const pid_t c = ::fork();
  REQUIRE(c >= 0);
  if (c == 0) {
    std::string err;
    if (!snowglobe::isolate::installIsolateFilter(err)) {
      _exit(10);
    }
    ::syscall(SYS_io_uring_setup, 8, nullptr);
    _exit(11); // must not survive the call
  }
  int st = 0;
  REQUIRE(::waitpid(c, &st, 0) == c);
  CHECK(WIFSIGNALED(st));
  CHECK(WTERMSIG(st) == SIGSYS);
#else
  SUCCEED("no SYS_io_uring_setup on this arch");
#endif
}

TEST_CASE("seccomp x32 numbers are denied (R5)", "[isolate]") {
#ifdef __linux__
  // probe/x32.c prints ENABLED on CONFIG_X86_X32_ABI=y kernels (this dev
  // box: ENABLED). x32 reuses the native audit arch with bit 30 set in nr,
  // so every nr-JEQ misses -> ALLOW without a range rule (proven bypass:
  // x32 add_key returned a live key serial under the pre-R5 filter). The
  // JGE range rule must EPERM any nr with the bit, on any arch (no
  // legitimate nr reaches it on x86_64 or aarch64).
  const pid_t c = ::fork();
  REQUIRE(c >= 0);
  if (c == 0) {
    std::string err;
    if (!snowglobe::isolate::installIsolateFilter(err)) {
      _exit(10);
    }
#ifdef SYS_add_key
    errno = 0;
    const long r = ::syscall(static_cast<long>(SYS_add_key) | 0x40000000L, "user", "x32deny",
                             "payload", 7, -4);
    if (r != -1 || errno != EPERM) {
      _exit(11); // bypassed (key serial) or wrong errno
    }
#else
    _exit(12);
#endif
    _exit(0);
  }
  int st = 0;
  REQUIRE(::waitpid(c, &st, 0) == c);
  CHECK(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
#else
  SUCCEED("requires Linux");
#endif
}

TEST_CASE("seccomp arch mismatch kills (fail closed, no fail-open)", "[isolate]") {
  // The BPF LD-nr thread starts with an arch check that KILLs on mismatch.
  // Prove it: build the real filter for the WRONG arch, install in a child,
  // and show even getpid dies SIGSYS (fail closed, never fail open).
  const auto calls = snowglobe::isolate::isolateBlocklist();
  REQUIRE(!calls.empty());
#ifdef __linux__
  unsigned int wrong = 0;
#if defined(__x86_64__)
  // AUDIT_ARCH_AARCH64 = 0xC00000B7 (EM_AARCH64=183)
  wrong = 0xC00000B7U;
#elif defined(__aarch64__)
  // AUDIT_ARCH_X86_64 = 0xC000003E (EM_X86_64=62)
  wrong = 0xC000003EU;
#else
  SUCCEED("unsupported arch for this test");
  return;
#endif
  const auto prog = snowglobe::isolate::buildIsolateFilterWithArch(calls, wrong);
  REQUIRE(!prog.empty());
  const pid_t c = ::fork();
  REQUIRE(c >= 0);
  if (c == 0) {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      _exit(10);
    }
    struct sock_fprog {
      unsigned short len;
      struct sock_filter* filter;
    };
    // Raw seccomp(2): SECCOMP_SET_MODE_FILTER=1. Use syscall directly to
    // avoid libseccomp dependency (same as production installer).
    struct sock_fprog fprog = {};
    fprog.len = static_cast<unsigned short>(prog.size());
    fprog.filter = const_cast<struct sock_filter*>(prog.data());
#ifdef SYS_seccomp
    if (::syscall(SYS_seccomp, 1, 0, &fprog) != 0) {
      _exit(11);
    }
#else
    _exit(12);
#endif
    // Any syscall now must die (arch mismatch -> KILL_PROCESS).
    (void)::getpid();
    _exit(13); // must not survive
  }
  int st = 0;
  REQUIRE(::waitpid(c, &st, 0) == c);
  CHECK(WIFSIGNALED(st));
  CHECK(WTERMSIG(st) == SIGSYS);
#endif
}

TEST_CASE("seccomp arg filtering: clone/socket/personality", "[isolate]") {
#ifdef __linux__
  const pid_t c = ::fork();
  REQUIRE(c >= 0);
  if (c == 0) {
    std::string err;
    if (!snowglobe::isolate::installIsolateFilter(err)) {
      _exit(10);
    }
    // clone without NS flags must WORK (thread/process creation).
    // Use fork() (separate syscall, always allowed) + a pthread-style
    // clone with only SIGCHLD (no NS bits).
#ifdef SYS_clone
    {
      // child func that immediately exits; stack needed for clone.
      static char stack[65536];
      auto fn = [](void*) -> int { return 0; };
      // Raw clone(SIGHLD|CLONE_VM|CLONE_FS|CLONE_FILES is for threads, but
      // needs shared memory; simpler: clone with SIGCHLD only (like fork).
      // If clone is broken, this fails and we _exit(11).
      const pid_t t = ::syscall(SYS_clone, SIGCHLD, nullptr, nullptr, nullptr, 0);
      if (t < 0) {
        // ENOSYS means clone missing (aarch64 has no clone? it does), fail.
        _exit(11);
      } else if (t == 0) {
        _exit(0); // child
      } else {
        int s = 0;
        ::waitpid(t, &s, 0);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) {
          _exit(12);
        }
      }
      (void)fn;
      (void)stack;
    }
#endif
    // unshare without NS (e.g. 0) must not EPERM-by-filter (may still fail
    // for other reasons, but not EPERM from us? unshare(0) succeeds).
#ifdef SYS_unshare
    {
      errno = 0;
      const int r = ::syscall(SYS_unshare, 0);
      if (r != 0) {
        _exit(13);
      }
    }
#endif
    // socket AF_INET must WORK.
    {
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) {
        _exit(14);
      }
      ::close(fd);
    }
    // personality query (0xffffffff) must WORK (Docker allows it).
#ifdef SYS_personality
    {
      errno = 0;
      const long r = ::syscall(SYS_personality, 0xffffffffUL);
      if (r < 0) {
        _exit(15);
      }
    }
#endif
    _exit(0);
  }
  int st = 0;
  REQUIRE(::waitpid(c, &st, 0) == c);
  CHECK(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
#else
  SUCCEED("requires Linux");
#endif
}

TEST_CASE("seccomp arg filtering denies NS/ALG/bad-personality", "[isolate]") {
#ifdef __linux__
  const pid_t c = ::fork();
  REQUIRE(c >= 0);
  if (c == 0) {
    std::string err;
    if (!snowglobe::isolate::installIsolateFilter(err)) {
      _exit(10);
    }
    // unshare(CLONE_NEWNS) must EPERM (filter, not caps — child is
    // unprivileged here too, but the filter is the deterministic cause;
    // the isolate_basic.py test proves it as mapped root).
#ifdef SYS_unshare
    {
      errno = 0;
      const long r = ::syscall(SYS_unshare, 0x00020000); // CLONE_NEWNS
      if (r != -1 || errno != EPERM) {
        _exit(11);
      }
    }
#endif
    // socket(AF_ALG) must EPERM.
    {
      errno = 0;
      const int fd = ::socket(38, SOCK_SEQPACKET, 0); // AF_ALG=38
      if (fd >= 0) {
        ::close(fd);
        _exit(12);
      }
      if (errno != EPERM) {
        _exit(13);
      }
    }
    // socket(AF_VSOCK) must EPERM.
    {
      errno = 0;
      const int fd = ::socket(40, SOCK_STREAM, 0); // AF_VSOCK=40
      if (fd >= 0) {
        ::close(fd);
        _exit(14);
      }
      if (errno != EPERM) {
        _exit(15);
      }
    }
    // personality(PER_BSD=1, not in Docker allow list) must EPERM.
#ifdef SYS_personality
    {
      errno = 0;
      const long r = ::syscall(SYS_personality, 1UL);
      if (r != -1 || errno != EPERM) {
        _exit(16);
      }
    }
#endif
    // clone3 must ENOSYS (not EPERM) so glibc falls back to clone.
#ifdef SYS_clone3
    {
      errno = 0;
      struct clone_args {
        unsigned long long flags;
        unsigned long long pidfd;
        unsigned long long child_tid;
        unsigned long long parent_tid;
        unsigned long long exit_signal;
        unsigned long long stack;
        unsigned long long stack_size;
        unsigned long long tls;
        unsigned long long set_tid;
        unsigned long long set_tid_size;
        unsigned long long cgroup;
      };
      struct clone_args a = {};
      a.exit_signal = 17; // SIGCHLD
      const long r = ::syscall(SYS_clone3, &a, sizeof(a));
      if (r != -1 || errno != ENOSYS) {
        _exit(17);
      }
    }
#endif
    _exit(0);
  }
  int st = 0;
  REQUIRE(::waitpid(c, &st, 0) == c);
  CHECK(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
#else
  SUCCEED("requires Linux");
#endif
}

TEST_CASE("defaultSecretMasks respects allow-path", "[isolate]") {
  using snowglobe::isolate::defaultSecretMasks;
  const auto m0 = defaultSecretMasks("/home/u", {});
  CHECK(m0.size() == 3);
  CHECK(m0[0] == "/home/u/.ssh");
  CHECK(m0[1] == "/home/u/.aws");
  CHECK(m0[2] == "/home/u/.gnupg");
  const auto m1 = defaultSecretMasks("/home/u", {"/home/u/.ssh"});
  CHECK(m1.size() == 2);
  CHECK(m1[0] == "/home/u/.aws");
  const auto m2 = defaultSecretMasks("/home/u", {"/home/u"});
  CHECK(m2.empty());
  CHECK(defaultSecretMasks("", {}).empty());
}

TEST_CASE("isSecretName matches the redact name rule", "[isolate]") {
  using snowglobe::isolate::isSecretName;
  CHECK(isSecretName("OPENAI_API_KEY"));
  CHECK(isSecretName("aws_secret_access_key"));
  CHECK(isSecretName("GITHUB_TOKEN"));
  CHECK(isSecretName("MY_PASSWORD"));
  CHECK(isSecretName("db_passwd"));
  CHECK(isSecretName("MY_CREDENTIAL"));
  CHECK(!isSecretName("ANTHROPIC_BASE_URL"));
  CHECK(!isSecretName("NORMAL_VAR"));
  CHECK(!isSecretName("HOSTNAME"));
  CHECK(!isSecretName("KEYBOARD")); // suffix, not substring
}

TEST_CASE("parseMemSize units", "[isolate]") {
  using snowglobe::isolate::parseMemSize;
  long long v = 0;
  std::string e;
  CHECK(parseMemSize("max", v, e));
  CHECK(v == -1);
  CHECK(parseMemSize("512", v, e));
  CHECK(v == 512);
  CHECK(parseMemSize("64M", v, e));
  CHECK(v == 64LL * 1024 * 1024);
  CHECK(parseMemSize("2G", v, e));
  CHECK(v == 2LL * 1024 * 1024 * 1024);
  CHECK(!parseMemSize("10X", v, e));
  CHECK(!parseMemSize("", v, e));
  CHECK(!parseMemSize("M", v, e));
}

TEST_CASE("landlock helper jails a child: RO denied, RW allowed", "[isolate]") {
  char roTemplate[] = "/tmp/sg-ll-ro-XXXXXX";
  char rwTemplate[] = "/tmp/sg-ll-rw-XXXXXX";
  REQUIRE(::mkdtemp(roTemplate) != nullptr);
  REQUIRE(::mkdtemp(rwTemplate) != nullptr);
  const pid_t c = ::fork();
  REQUIRE(c >= 0);
  if (c == 0) {
    // restrict_self requires no_new_privs on this kernel (EPERM without).
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      _exit(14);
    }
    std::string err;
    if (!snowglobe::isolate::enforceLandlock({rwTemplate}, err)) {
      _exit(10);
    }
    // RW island works.
    const std::string ok = std::string(rwTemplate) + "/f";
    const int fd = ::open(ok.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      _exit(11);
    }
    ::close(fd);
    // RO world denies.
    const std::string no = std::string(roTemplate) + "/f";
    errno = 0;
    const int fd2 = ::open(no.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd2 >= 0) {
      ::close(fd2);
      _exit(12);
    }
    if (errno != EPERM && errno != EACCES) {
      _exit(13);
    }
    _exit(0);
  }
  int st = 0;
  REQUIRE(::waitpid(c, &st, 0) == c);
  CHECK(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
  ::rmdir(roTemplate);
  ::rmdir(rwTemplate);
}

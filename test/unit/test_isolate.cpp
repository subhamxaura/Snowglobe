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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

#include "isolate/landlock.hpp"
#include "isolate/isolate.hpp"
#include "isolate/seccomp.hpp"

TEST_CASE("isolate blocklist shape", "[isolate]") {
  const auto calls = snowglobe::isolate::isolateBlocklist();
  CHECK(!calls.empty());
  bool mountErrno = false;
  bool uringKill = false;
  bool ptraceErrno = false;
  for (const auto& c : calls) {
    if (std::string(c.name) == "mount" && !c.kill) {
      mountErrno = true;
    }
    if (std::string(c.name) == "io_uring_setup" && c.kill) {
      uringKill = true;
    }
    if (std::string(c.name) == "ptrace" && !c.kill) {
      ptraceErrno = true;
    }
  }
  CHECK(mountErrno);
  CHECK(uringKill);
  CHECK(ptraceErrno);
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

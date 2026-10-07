// Landlock enforcement (ADR-0007): RO world, RW islands. Raw syscalls
// (no library); v1 rights subset for ABI>=1 kernels.
#include "landlock.hpp"

#ifdef __linux__
#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_landlock_create_ruleset
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif
#endif
// REFER (1<<13) and TRUNCATE (1<<14) postdate older libc headers
// (e.g. Ubuntu 22.04 CI); the runtime ABI query still gates use.
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

namespace snowglobe::isolate {
namespace {

// v1 access rights (from linux/landlock.h; pinned fallbacks match).
uint64_t roRights() {
  return LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
}

uint64_t rwRights() {
  return roRights() | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
         LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
         LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
         LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
         LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
}

bool addPathRule(int ruleset, const char* path, uint64_t rights, std::string& error) {
  const int fd = ::open(path, O_PATH | O_CLOEXEC);
  if (fd < 0) {
    error = std::string("isolate: landlock open ") + path + ": " + ::strerror(errno);
    return false;
  }
  struct landlock_path_beneath_attr attr = {};
  attr.parent_fd = fd;
  attr.allowed_access = (__u64)rights;
  const int rc = ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &attr, 0);
  const int e = errno;
  ::close(fd);
  if (rc != 0) {
    error = std::string("isolate: landlock add_rule ") + path + ": " + ::strerror(e);
    return false;
  }
  return true;
}

} // namespace

long landlockQueryAbi() {
#ifdef SYS_landlock_create_ruleset
  errno = 0;
  const long abi =
      ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  return abi >= 0 ? abi : -errno;
#else
  return -ENOSYS;
#endif
}

bool enforceLandlock(const std::vector<std::string>& rwPaths, std::string& error) {
#ifdef SYS_landlock_create_ruleset
  const long abi = landlockQueryAbi();
  if (abi < 1) {
    error = std::string("isolate: landlock unavailable (abi query: ") + ::strerror((int)-abi) + ")";
    return false;
  }
  struct landlock_ruleset_attr attr = {};
  attr.handled_access_fs = (__u64)(roRights() | rwRights());
  const int ruleset = (int)::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
  if (ruleset < 0) {
    error = std::string("isolate: landlock ruleset: ") + ::strerror(errno);
    return false;
  }
  bool ok = addPathRule(ruleset, "/", roRights(), error);
  for (size_t i = 0; ok && i < rwPaths.size(); ++i) {
    ok = addPathRule(ruleset, rwPaths[i].c_str(), rwRights(), error);
  }
  if (ok && ::syscall(SYS_landlock_restrict_self, ruleset, 0) != 0) {
    error = std::string("isolate: landlock restrict: ") + ::strerror(errno);
    ok = false;
  }
  ::close(ruleset);
  return ok;
#else
  (void)rwPaths;
  error = "isolate: landlock unsupported on this arch";
  return false;
#endif
}

} // namespace snowglobe::isolate
#else

namespace snowglobe::isolate {

long landlockQueryAbi() {
  return -38;
} // -ENOSYS

bool enforceLandlock(const std::vector<std::string>& rwPaths, std::string& error) {
  (void)rwPaths;
  error = "isolate: requires Linux";
  return false;
}

} // namespace snowglobe::isolate
#endif

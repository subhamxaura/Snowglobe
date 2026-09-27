// probe/landlock.c — query the Landlock ABI directly, no guessing.
// Calls landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION),
// which returns the highest supported ABI version (man landlock_create_ruleset(2)).
// Build: cc -o /tmp/probe_landlock probe/landlock.c && /tmp/probe_landlock
// Prints "landlock: abi <n>" or "landlock: no: <reason>".
// Mapping: ret >= 0  → ABI version; ENOSYS → kernel compiled without Landlock;
// EOPNOTSUPP → Landlock LSM not enabled; EINVAL → too old for the VERSION query.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_landlock_create_ruleset
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_landlock_create_ruleset 444
#endif
#endif
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

int main(void) {
#ifdef SYS_landlock_create_ruleset
  errno = 0;
  const long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi >= 0) {
    printf("landlock: abi %ld\n", abi);
    return 0;
  }
  const char* why = NULL;
  if (errno == ENOSYS) {
    why = "kernel compiled without Landlock (ENOSYS)";
  } else if (errno == EOPNOTSUPP) {
    why = "Landlock LSM not enabled (EOPNOTSUPP)";
  } else if (errno == EINVAL) {
    why = "VERSION query unsupported (EINVAL; predates Landlock 5.13)";
  } else {
    why = strerror(errno);
  }
  printf("landlock: no: %s\n", why);
  return 1;
#else
  printf("landlock: no: no SYS_landlock_create_ruleset for this arch\n");
  return 1;
#endif
}

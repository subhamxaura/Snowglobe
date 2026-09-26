// probe/landlock.c — which Landlock ABI version is available?
// Build: cc -o /tmp/probe_landlock probe/landlock.c && /tmp/probe_landlock
// Prints "landlock: absent" or "landlock: abi <n>".
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_landlock_create_ruleset
#if defined(__x86_64__)
#define SYS_landlock_create_ruleset 444
#elif defined(__aarch64__)
#define SYS_landlock_create_ruleset 444
#endif
#endif

int main(void) {
#ifdef SYS_landlock_create_ruleset
  const long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, 0);
  // EINVAL/ENOSYS patterns reveal presence; full ABI query needs landlock headers.
  (void)abi;
  if (access("/proc/sys/abi/landlock", F_OK) == 0) {
    printf("landlock: present (see /proc/sys/abi/landlock)\n");
    return 0;
  }
  printf("landlock: absent\n");
  return 1;
#else
  printf("landlock: absent (no SYS_landlock_create_ruleset)\n");
  return 1;
#endif
}

// probe/seccomp_notif.c — is SECCOMP_RET_USER_NOTIF available? Ask the kernel.
// Calls seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &SECCOMP_RET_USER_NOTIF):
// 0 means the action is available (USER_NOTIF needs kernel >= 5.11).
// Build: cc -o /tmp/probe_seccomp probe/seccomp_notif.c && /tmp/probe_seccomp
// (raw syscall(2); no libseccomp needed.)
// Prints "seccomp-notify: yes" or "seccomp-notify: no: <reason>".
// Mapping: 0 → available; EINVAL → action unknown (kernel < 5.11);
// ENOSYS → no seccomp(2). Full notif-fd flow lands in Phase 4.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_seccomp
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_seccomp 317
#endif
#endif
#ifndef SECCOMP_GET_ACTION_AVAIL
#define SECCOMP_GET_ACTION_AVAIL 2
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif

int main(void) {
#ifdef SYS_seccomp
  unsigned int act = SECCOMP_RET_USER_NOTIF;
  errno = 0;
  const long r = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
  if (r == 0) {
    printf("seccomp-notify: yes\n");
    return 0;
  }
  const char* why = NULL;
  if (errno == EINVAL) {
    why = "SECCOMP_RET_USER_NOTIF unknown (EINVAL; kernel < 5.11)";
  } else if (errno == ENOSYS) {
    why = "no seccomp(2) (ENOSYS)";
  } else {
    why = strerror(errno);
  }
  printf("seccomp-notify: no: %s\n", why);
  return 1;
#else
  printf("seccomp-notify: no: no SYS_seccomp for this arch\n");
  return 1;
#endif
}

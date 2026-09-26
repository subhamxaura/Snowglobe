// probe/seccomp_notif.c — is seccomp user-notification (5.11+) usable?
// Build: cc -o /tmp/probe_seccomp probe/seccomp_notif.c -lseccomp && /tmp/probe_seccomp
// Prints "seccomp-notify: yes|no". Full notif-fd flow lands in Phase 4.
#include <stdio.h>

#ifdef __linux__
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

int main(void) {
#ifdef __linux__
  // SECCOMP_USER_NOTIF_FLAG_CONTINUE is 5.11+; presence of the constant
  // plus kernel >= 5.11 implies availability (runtime check in doctor).
  FILE* f = fopen("/proc/version", "r");
  char ver[256] = {};
  if (f != NULL) {
    if (fgets(ver, sizeof(ver), f) == NULL) {
      ver[0] = '\0';
    }
    fclose(f);
  }
  int major = 0, minor = 0;
  sscanf(ver, "Linux version %d.%d", &major, &minor);
  if (major > 5 || (major == 5 && minor >= 11)) {
    printf("seccomp-notify: yes (kernel %d.%d)\n", major, minor);
    return 0;
  }
  printf("seccomp-notify: no (kernel %d.%d < 5.11)\n", major, minor);
  return 1;
#else
  printf("seccomp-notify: no (non-Linux)\n");
  return 1;
#endif
}

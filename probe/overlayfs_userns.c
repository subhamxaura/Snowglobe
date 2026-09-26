// probe/overlayfs_userns.c — overlayfs mount inside a user namespace?
// Build: cc -o /tmp/probe_overlay probe/overlayfs_userns.c && /tmp/probe_overlay
// Must run unprivileged; prints "overlayfs-in-userns: yes|no: <reason>".
// Result recorded in docs/adr/0002-* (Phase 2.1).
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int childFn(void* arg) {
  (void)arg;
  mkdir("/tmp/sg-lower", 0755);
  mkdir("/tmp/sg-upper", 0755);
  mkdir("/tmp/sg-work", 0755);
  mkdir("/tmp/sg-merged", 0755);
  if (mount("overlay", "/tmp/sg-merged", "overlay", 0,
            "lowerdir=/tmp/sg-lower,upperdir=/tmp/sg-upper,workdir=/tmp/sg-work") != 0) {
    perror("mount overlay");
    return 1;
  }
  umount("/tmp/sg-merged");
  return 0;
}

int main(void) {
  char stack[65536];
  char* args[] = {NULL};
  (void)args;
  // New user + mount ns; uid map left to parent via /proc/PID/uid_map by caller.
  const pid_t pid = clone(childFn, stack + sizeof(stack),
                          CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD, NULL);
  if (pid < 0) {
    perror("clone");
    printf("overlayfs-in-userns: no: clone failed\n");
    return 1;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    printf("overlayfs-in-userns: yes\n");
    return 0;
  }
  printf("overlayfs-in-userns: no: child status %d (check uid_map setup)\n", status);
  return 1;
}

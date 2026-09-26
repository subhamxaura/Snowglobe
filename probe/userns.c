// probe/userns.c — can we create a user namespace? (AGENTS.md §9)
// Build: cc -o /tmp/probe_userns probe/userns.c && /tmp/probe_userns
// Expected on a capable box: "userns: yes".
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int childFn(void* arg) {
  (void)arg;
  return 0;
}

int main(void) {
  char stack[65536];
  // NOLINTNEXTLINE: clone with CLONE_NEWUSER probes userns support.
  const pid_t pid = clone(childFn, stack + sizeof(stack), CLONE_NEWUSER | SIGCHLD, NULL);
  if (pid < 0) {
    perror("userns");
    printf("userns: no\n");
    return 1;
  }
  waitpid(pid, NULL, 0);
  printf("userns: yes\n");
  return 0;
}

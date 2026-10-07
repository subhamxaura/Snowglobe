// probe/ptrace_pidns.c — SUPERSEDED, do not run directly (the hand-rolled
// tracer loop below hangs on a userspace event-handling bug, NOT a kernel
// limitation: it mishandles the fork-event/stop sequence and deadlocks).
// The question it asked is ANSWERED: cross-pidns tracing works —
// probe/ns_harness.c under the real `snowglobe run` shows the full
// middle/init/agent chain with exit codes (the tracer already waits with
// __WALL). Kept as documentation of the investigation, not as a tool.
// Build: cc -o /tmp/ppidns probe/ptrace_pidns.c && /tmp/ppidns
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/ptrace.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x00000008
#endif
#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL 0x00100000
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420f
#endif

static int fails = 0;
#define CHECK(c, msg)                                                                              \
  do {                                                                                             \
    if (!(c)) {                                                                                    \
      printf("FAIL: %s: %s\n", msg, strerror(errno));                                              \
      fails = 1;                                                                                   \
      goto done;                                                                                   \
    } else {                                                                                       \
      printf("ok: %s\n", msg);                                                                     \
    }                                                                                              \
  } while (0)

static int pput(const char* path, const char* s) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return -1;
  size_t n = strlen(s), w = 0;
  while (w < n) {
    ssize_t k = write(fd, s + w, n - w);
    if (k < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      return -1;
    }
    w += (size_t)k;
  }
  close(fd);
  return 0;
}

static void middle(int reqFd, int ackFd) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (unshare(CLONE_NEWUSER) != 0) {
    printf("FAIL: unshare USER: %s\n", strerror(errno));
    _exit(10);
  }
  if (write(reqFd, "M", 1) != 1) {
    printf("FAIL: map req\n");
    _exit(11);
  }
  char ack = 0;
  if (read(ackFd, &ack, 1) != 1 || ack != 'G') {
    printf("FAIL: map ack\n");
    _exit(12);
  }
  if (unshare(CLONE_NEWPID) != 0) {
    printf("FAIL: unshare PID: %s\n", strerror(errno));
    _exit(13);
  }
  if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) {
    printf("FAIL: TRACEME: %s\n", strerror(errno));
    _exit(14);
  }
  raise(SIGSTOP);
  pid_t init = fork();
  if (init < 0)
    _exit(15);
  if (init == 0) {
    char* av[] = {(char*)"/bin/true", NULL};
    execv("/bin/true", av);
    _exit(127);
  }
  int st = 0, code = 99;
  while (waitpid(-1, &st, 0) > 0) {
    if (WIFEXITED(st))
      code = WEXITSTATUS(st);
  }
  _exit(code);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int toC[2], toP[2];
  CHECK(pipe(toC) == 0 && pipe(toP) == 0, "pipes");
  pid_t m = fork();
  CHECK(m >= 0, "fork");
  if (m == 0) {
    close(toC[1]);
    close(toP[0]);
    middle(toP[1], toC[0]);
    _exit(127);
  }
  close(toC[0]);
  close(toP[1]);
  char req = 0;
  CHECK(read(toP[0], &req, 1) == 1 && req == 'M', "map request");
  char pgm[64], ump[64], gmp[64], umap[64], gmap[64];
  snprintf(pgm, sizeof pgm, "/proc/%d/setgroups", (int)m);
  snprintf(ump, sizeof ump, "/proc/%d/uid_map", (int)m);
  snprintf(gmp, sizeof gmp, "/proc/%d/gid_map", (int)m);
  snprintf(umap, sizeof umap, "0 %d 1", (int)getuid());
  snprintf(gmap, sizeof gmap, "0 %d 1", (int)getgid());
  pput(pgm, "deny");
  CHECK(pput(ump, umap) == 0 && pput(gmp, gmap) == 0, "parent maps");
  CHECK(write(toC[1], "G", 1) == 1, "ack");

  int st = 0;
  CHECK(waitpid(m, &st, 0) == m && WIFSTOPPED(st), "first SIGSTOP");
  long opts = PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD;
  CHECK(ptrace(PTRACE_SETOPTIONS, m, NULL, (void*)opts) == 0, "SETOPTIONS");
  CHECK(ptrace(PTRACE_CONT, m, NULL, NULL) == 0, "CONT");
  int grand = -1, decoded = 0, code = -1;
  for (int i = 0; i < 1000; i++) {
    // __WALL: tracees need not be our children (cloned grandchildren,
    // pid namespaces) — same flag the real tracer uses.
    pid_t w = waitpid(-1, &st, __WALL);
    printf("wait: w=%d st=0x%x ev=%u sig=%d\n", (int)w, (unsigned)st,
           (unsigned)((st >> 16) & 0xffff), WSTOPSIG(st));
    if (w < 0 && errno == ECHILD)
      break;
    if (w < 0 && errno == ECHILD)
      break;
    CHECK(w >= 0, "wait");
    if (WIFEXITED(st) || WIFSIGNALED(st)) {
      if (w == m)
        code = WIFEXITED(st) ? WEXITSTATUS(st) : 300;
      continue;
    }
    if (!WIFSTOPPED(w))
      continue;
    int sig = WSTOPSIG(st);
    unsigned ev = (unsigned)((st >> 16) & 0xffff);
    if (ev == PTRACE_EVENT_CLONE || ev == PTRACE_EVENT_FORK || ev == PTRACE_EVENT_VFORK) {
      unsigned long msg = 0;
      CHECK(ptrace(PTRACE_GETEVENTMSG, w, NULL, &msg) == 0, "GETEVENTMSG");
      if ((pid_t)msg != m)
        grand = (int)msg;
      CHECK(ptrace(PTRACE_CONT, w, NULL, NULL) == 0, "CONT clone");
      continue;
    }
    if (sig == (SIGTRAP | 0x80) && grand > 0 && w == grand && !decoded) {
      struct ptrace_syscall_info info;
      memset(&info, 0, sizeof info);
      errno = 0;
      if (ptrace(PTRACE_GET_SYSCALL_INFO, w, (void*)sizeof info, &info) >= 0)
        decoded = 1;
      else
        printf("note: GET_SYSCALL_INFO grandchild errno: %s\n", strerror(errno));
      CHECK(ptrace(PTRACE_SYSCALL, w, NULL, NULL) == 0, "SYSCALL cont");
      continue;
    }
    int d = (sig == SIGSTOP || sig == SIGTRAP) ? 0 : sig;
    CHECK(ptrace(PTRACE_CONT, w, NULL, (void*)(long)d) == 0, "CONT");
  }
  CHECK(grand > 0, "grandchild visible across pidns");
  CHECK(decoded, "syscall decoded across pidns");
  CHECK(code == 0, "exit code propagated");
done:
  if (!fails)
    printf("ALL-PASS ptrace_pidns\n");
  return fails ? 1 : 0;
}

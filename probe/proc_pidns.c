// probe/proc_pidns.c — follow-up (a): was the proc EPERM caused by
// mounting OUTSIDE the pidns? Retry as PID 1: unshare USER (parent
// mapped) + PID, fork; the child (pid 1 in the new ns) mounts proc on a
// fresh dir. EPERM here too => kernel policy, keep the tmpfs fallback.
// Build: cc -o /tmp/procpidns probe/proc_pidns.c && /tmp/procpidns
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int pput(const char* path, const char* s) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  size_t n = strlen(s), w = 0;
  while (w < n) {
    ssize_t k = write(fd, s + w, n - w);
    if (k < 0) { if (errno == EINTR) continue; close(fd); return -1; }
    w += (size_t)k;
  }
  close(fd);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int toC[2], toP[2];
  if (pipe(toC) || pipe(toP)) { printf("pipe: %s\n", strerror(errno)); return 1; }
  pid_t m = fork();
  if (m < 0) { printf("fork: %s\n", strerror(errno)); return 1; }
  if (m == 0) {
    close(toC[1]); close(toP[0]);
    if (unshare(CLONE_NEWUSER) != 0) { printf("child unshare: %s\n", strerror(errno)); _exit(10); }
    if (write(toP[1], "M", 1) != 1) _exit(11);
    char ack = 0;
    if (read(toC[0], &ack, 1) != 1 || ack != 'G') { printf("child no ack\n"); _exit(12); }
    if (unshare(CLONE_NEWPID) != 0) { printf("child pidns: %s\n", strerror(errno)); _exit(13); }
    pid_t init = fork();
    if (init < 0) { printf("child fork: %s\n", strerror(errno)); _exit(14); }
    if (init == 0) {
      // I am PID 1 of the new ns (getpid()==1): mount proc now.
      printf("init pid=%d\n", (int)getpid());
      mkdir("/tmp/pp1", 0755);
      if (mount("proc", "/tmp/pp1", "proc", 0, "") != 0) {
        printf("PID1 proc mount: %s\n", strerror(errno));
        _exit(15);
      }
      printf("PID1 proc mount: OK\n");
      _exit(0);
    }
    int st = 0;
    waitpid(init, &st, 0);
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 20);
  }
  close(toC[0]); close(toP[1]);
  char req = 0;
  if (read(toP[0], &req, 1) != 1) { printf("parent no req\n"); return 2; }
  char pgm[64], ump[64], gmp[64], umap[64], gmap[64];
  snprintf(pgm, sizeof pgm, "/proc/%d/setgroups", (int)m);
  snprintf(ump, sizeof ump, "/proc/%d/uid_map", (int)m);
  snprintf(gmp, sizeof gmp, "/proc/%d/gid_map", (int)m);
  snprintf(umap, sizeof umap, "0 %d 1", (int)getuid());
  snprintf(gmap, sizeof gmap, "0 %d 1", (int)getgid());
  pput(pgm, "deny");
  if (pput(ump, umap) || pput(gmp, gmap)) { printf("parent maps failed\n"); return 3; }
  if (write(toC[1], "G", 1) != 1) { printf("parent no ack\n"); return 4; }
  int st = 0;
  waitpid(m, &st, 0);
  printf("middle exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

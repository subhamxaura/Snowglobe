// probe/ns_harness.c — namespace middle + PID-1 init-helper WITHOUT any
// ptrace logic. Run UNDER the real tracer to prove cross-namespace
// tracing end-to-end:
//
//   snowglobe run --out=/tmp/nstest.sgr -- /tmp/nsharness
//
// Parent (supervisor side here): maps the middle over a pipe, then
// waitpids everything. Middle: unshare USER+NS, mounts (selective:
// tmpfs root, ro rbinds, repo/etc overlays, fresh proc, tmpfs tmp,
// pivot), unshare PID, fork init-helper (PID 1, reaps, propagates
// code); middle waits and exits with init's code. Init-helper forks the
// agent `sh -c 'echo ns-ok; exit 7'` — exit 7 must propagate end to end.
// inspects: grandchild exec visible? exit 7 out the top? no zombies?
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

static int fails = 0;
#define CHECK(c, msg) do { \
  if (!(c)) { printf("FAIL: %s: %s\n", msg, strerror(errno)); fails = 1; goto done; } \
  else { printf("ok: %s\n", msg); } \
} while (0)

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

static void initHelper(void) {
  pid_t a = fork();
  if (a < 0) _exit(40);
  if (a == 0) {
    char* av[] = {(char*)"sh", (char*)"-c", (char*)"echo ns-ok; exit 7", NULL};
    execvp("sh", av);
    _exit(127);
  }
  int st = 0, code = 99;
  for (;;) {
    pid_t w = waitpid(-1, &st, 0);
    if (w < 0) break;
    if (w == a) code = WIFEXITED(st) ? WEXITSTATUS(st) : 100;
  }
  _exit(code);
}

static void middle(int reqFd, int ackFd) {
  if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) _exit(10);
  if (write(reqFd, "M", 1) != 1) _exit(11);
  char ack = 0;
  if (read(ackFd, &ack, 1) != 1 || ack != 'G') _exit(12);
  // NOTE: no mounts in this revision (mount layout is core/isolate's
  // job, proven by the overlay probe + implementation tests). Pure
  // userns + pidns + reaping shape only.
  if (unshare(CLONE_NEWPID) != 0) _exit(13);
  pid_t init = fork();
  if (init < 0) _exit(14);
  if (init == 0) { initHelper(); _exit(99); }
  int st = 0, code = 98;
  for (;;) {
    pid_t w = waitpid(-1, &st, 0);
    if (w < 0) break;
    if (w == init) code = WIFEXITED(st) ? WEXITSTATUS(st) : 90;
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
    close(toC[1]); close(toP[0]);
    middle(toP[1], toC[0]);
    _exit(127);
  }
  close(toC[0]); close(toP[1]);
  char req = 0;
  CHECK(read(toP[0], &req, 1) == 1 && req == 'M', "map request");
  char pgm[64], ump[64], gmp[64], umap[64], gmap[64];
  snprintf(pgm, sizeof pgm, "/proc/%d/setgroups", (int)m);
  snprintf(ump, sizeof ump, "/proc/%d/uid_map", (int)m);
  snprintf(gmp, sizeof gmp, "/proc/%d/gid_map", (int)m);
  snprintf(umap, sizeof umap, "0 %d 1", (int)getuid());
  snprintf(gmap, sizeof gmap, "0 %d 1", (int)getgid());
  pput(pgm, "deny");
  CHECK(pput(ump, umap) == 0 && pput(gmp, gmap) == 0, "maps");
  CHECK(write(toC[1], "G", 1) == 1, "ack");
  int st = 0, code = -1;
  for (;;) {
    pid_t w = waitpid(-1, &st, 0);
    if (w < 0) break;
    if (w == m) code = WIFEXITED(st) ? WEXITSTATUS(st) : 80;
  }
  printf("middle exit=%d (want 7)\n", code);
  CHECK(code == 7, "exit propagation");
done:
  if (!fails) printf("ALL-PASS ns_harness\n");
  return fails ? 1 : 0;
}

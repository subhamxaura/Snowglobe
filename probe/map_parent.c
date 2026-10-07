// probe/map_parent.c — who may write the uid_map after unshare(USER)?
// Variant test for the isolate_smoke EPERM finding. Parent forks, child
// unshares and blocks on a pipe; parent writes setgroups/uid_map/gid_map.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int toC[2], toP[2];
  if (pipe(toC) || pipe(toP)) {
    printf("pipe: %s\n", strerror(errno));
    return 1;
  }
  pid_t c = fork();
  if (c < 0) {
    printf("fork: %s\n", strerror(errno));
    return 1;
  }
  if (c == 0) {
    close(toC[1]);
    close(toP[0]);
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
      printf("child unshare: %s\n", strerror(errno));
      _exit(10);
    }
    char b = 0;
    if (write(toP[1], "M", 1) != 1)
      _exit(11);
    if (read(toC[0], &b, 1) != 1) {
      printf("child no ack: %s\n", strerror(errno));
      _exit(12);
    }
    // Am I mapped? getuid should now read 0.
    printf("child uid=%d gid=%d\n", (int)getuid(), (int)getgid());
    _exit(getuid() == 0 ? 0 : 13);
  }
  close(toC[0]);
  close(toP[1]);
  char b = 0;
  if (read(toP[0], &b, 1) != 1) {
    printf("parent no req\n");
    return 2;
  }
  char map[64], pgm[64];
  snprintf(pgm, sizeof pgm, "/proc/%d/setgroups", (int)c);
  snprintf(map, sizeof map, "/proc/%d/uid_map", (int)c);
  int e1 = pput(pgm, "deny") ? errno : 0;
  int e2 = pput(map, "0 1000 1") ? errno : 0;
  snprintf(map, sizeof map, "/proc/%d/gid_map", (int)c);
  snprintf(pgm, sizeof pgm, "/proc/%d/setgroups", (int)c);
  int e3 = pput(map, "0 1000 1") ? errno : 0;
  printf("parent map results: setgroups=%s uid_map=%s gid_map=%s\n", e1 ? strerror(e1) : "ok",
         e2 ? strerror(e2) : "ok", e3 ? strerror(e3) : "ok");
  if (write(toC[1], "G", 1) != 1) {
    printf("parent no ack\n");
    return 3;
  }
  int st = 0;
  waitpid(c, &st, 0);
  printf("child exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return (e2 == 0 && e3 == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
}

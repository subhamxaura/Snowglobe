// sg_threads.c — 4 threads with ordered milestones (see
// test/fixtures/scenarios/README.md for why milestones must be ordered).
// Usage: sg_threads <dir>. t0 runs at once; t1..t3 block on forward pipes.
// Each thread opens its own file, signals the next, then blocks on its ack
// pipe until its successor finishes — opens land f0..f3, deaths t3..t0.
// Main polls file existence between creations (stat is never decoded, so the
// polls are invisible) so proc.start order is total; threads still overlap
// while blocked in read. Exits nonzero with perror on any failure.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_dir[1024];

struct Spec {
  int i;
  int wait_fd;
  int signal_fd;
  int ack_wait;
  int ack_sig;
};

static void* work(void* arg) {
  const struct Spec* s = (const struct Spec*)arg;
  char b = 0;
  if (s->wait_fd >= 0 && read(s->wait_fd, &b, 1) != 1) {
    return NULL;
  }
  char path[1152];
  snprintf(path, sizeof(path), "%s/t%d", g_dir, s->i);
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return NULL;
  }
  char msg[32];
  snprintf(msg, sizeof(msg), "thread %d\n", s->i);
  (void)!write(fd, msg, strlen(msg));
  close(fd);
  if (s->signal_fd >= 0 && write(s->signal_fd, "x", 1) != 1) {
    return NULL;
  }
  if (s->ack_wait >= 0 && read(s->ack_wait, &b, 1) != 1) {
    return NULL;
  }
  if (s->ack_sig >= 0 && write(s->ack_sig, "x", 1) != 1) {
    return NULL;
  }
  return NULL;
}

static int exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

int main(int argc, char** argv) {
  if (argc != 2 || strlen(argv[1]) >= sizeof(g_dir)) {
    fprintf(stderr, "usage: sg_threads <dir>\n");
    return 2;
  }
  strcpy(g_dir, argv[1]);

  int fwd[3][2], ack[3][2];
  for (int i = 0; i < 3; i++) {
    if (pipe(fwd[i]) != 0 || pipe(ack[i]) != 0) {
      perror("pipe");
      return 1;
    }
  }
  struct Spec specs[4] = {
      {0, -1, fwd[0][1], ack[0][0], -1},
      {1, fwd[0][0], fwd[1][1], ack[1][0], ack[0][1]},
      {2, fwd[1][0], fwd[2][1], ack[2][0], ack[1][1]},
      {3, fwd[2][0], -1, -1, ack[2][1]},
  };
  pthread_t t[4];
  for (int i = 0; i < 4; i++) {
    if (pthread_create(&t[i], NULL, work, &specs[i]) != 0) {
      perror("pthread_create");
      return 1;
    }
    char path[1152];
    snprintf(path, sizeof(path), "%s/t%d", g_dir, i);
    while (!exists(path)) {
      // spin on stat: invisible to the tracer, orders creation milestones
    }
  }
  for (int i = 0; i < 4; i++) {
    pthread_join(t[i], NULL);
  }
  printf("threads: done\n");
  return 0;
}

// sg_threads.c — 4 threads with ordered milestones (see
// test/fixtures/scenarios/README.md for why milestones must be ordered).
// Usage: sg_threads <dir>. t0 runs at once; t1..t3 block on forward pipes.
// Each thread opens its own file, signals the next via fwd, then blocks on
// its exit gate until main allows it to die — opens land f0..f3, deaths
// t3..t0 with no overlap. Main polls file existence between creations (stat
// is never decoded, so the polls are invisible) so proc.start order is
// total; threads still overlap while blocked in read. Exits nonzero with
// perror on any failure.
//
// Why main-gated exits (not ack pipes): an ack chain where each thread
// signals the next *then exits* is racy — successor wake/exit is concurrent
// with predecessor exit, so waitpid order varies with observer speed (ASan
// slowdown flipped it on 22.04). Here each exit is causally after the
// successor's *join* (main joins t3 before signalling t2, etc.), giving a
// total real-time order t3,t2,t1,t0 regardless of observer speed.
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
  int gate_fd; // exit permission from main (-1 for t3: exits immediately)
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
  // Main-gated exit: t3 has no gate (exits at once); t0..t2 wait until main
  // has joined their successor, so deaths are strictly t3..t0.
  if (s->gate_fd >= 0 && read(s->gate_fd, &b, 1) != 1) {
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

  int fwd[3][2], gate[3][2];
  for (int i = 0; i < 3; i++) {
    if (pipe(fwd[i]) != 0 || pipe(gate[i]) != 0) {
      perror("pipe");
      return 1;
    }
  }
  // gate[0] -> t0, gate[1] -> t1, gate[2] -> t2; t3 exits immediately.
  struct Spec specs[4] = {
      {0, -1, fwd[0][1], gate[0][0]},
      {1, fwd[0][0], fwd[1][1], gate[1][0]},
      {2, fwd[1][0], fwd[2][1], gate[2][0]},
      {3, fwd[2][0], -1, -1},
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
  // Total exit order: join t3 (already exiting), then release t2..t0 in turn.
  // Each join returns only after that thread's death, so the next thread's
  // death is strictly later — waitpid order is deterministic.
  if (pthread_join(t[3], NULL) != 0) {
    perror("pthread_join t3");
    return 1;
  }
  for (int i = 2; i >= 0; i--) {
    if (write(gate[i][1], "x", 1) != 1) {
      perror("gate write");
      return 1;
    }
    if (pthread_join(t[i], NULL) != 0) {
      perror("pthread_join");
      return 1;
    }
  }
  printf("threads: done\n");
  return 0;
}

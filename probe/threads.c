// probe/threads.c — 4 pthreads, each open(2)s a distinct file, then join.
// Proves per-tid attribution (Block 1.1): every event must carry pid (=tgid)
// and tid, proc.start for threads sets thread:true.
// Build: cc -o /tmp/probe_threads probe/threads.c -pthread
// Run: ./build/debug/core/snowglobe run --out=/tmp/thr.sgr -- /tmp/probe_threads
// Expect: 4 proc.start with thread:true + 4 fs.open, all tids distinct.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static void* worker(void* arg) {
  char path[64];
  snprintf(path, sizeof(path), "/tmp/sg-thread-%ld", (long)arg);
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    (void)!write(fd, "x", 1);
    close(fd);
  }
  return NULL;
}

int main(void) {
  pthread_t t[4];
  for (long i = 0; i < 4; i++) {
    pthread_create(&t[i], NULL, worker, (void*)i);
  }
  for (int i = 0; i < 4; i++) {
    pthread_join(t[i], NULL);
  }
  printf("threads: done\n");
  return 0;
}

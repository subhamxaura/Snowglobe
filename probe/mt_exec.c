// probe/mt_exec.c — main thread spawns a sleeping worker, then execs /bin/echo.
// Under ptrace (ptrace(2) "execve under ptrace"): the non-execing threads
// vanish with NO exit stops; the execing thread survives (possibly with a new
// tid==tgid identity). Snowglobe must emit proc.exit for the vanished tids
// plus exactly one proc.exec (Block 1.2).
// Build: cc -o /tmp/probe_mt_exec probe/mt_exec.c -pthread
// Run: ./build/debug/core/snowglobe run --out=/tmp/mte.sgr -- /tmp/probe_mt_exec
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static void* worker(void* arg) {
  (void)arg;
  sleep(30);
  return NULL;
}

int main(void) {
  pthread_t t;
  pthread_create(&t, NULL, worker, NULL);
  sleep(1);  // let the worker appear in the traced tree
  execl("/bin/echo", "echo", "exec-ok", NULL);
  perror("execl");
  return 127;
}

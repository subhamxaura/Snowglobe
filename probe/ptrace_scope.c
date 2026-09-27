// probe/ptrace_scope.c — YAMA ptrace_scope value (0 = classic, 1+ = restricted).
// Build: cc -o /tmp/probe_ptrace probe/ptrace_scope.c && /tmp/probe_ptrace
#include <stdio.h>

int main(void) {
  FILE* f = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
  if (f == NULL) {
    printf("ptrace_scope: unreadable (YAMA disabled or non-Linux)\n");
    return 1;
  }
  int v = -1;
  if (fscanf(f, "%d", &v) != 1) {
    v = -1;
  }
  fclose(f);
  printf("ptrace_scope: %d%s\n", v,
         v <= 1 ? " (tracing child processes OK)" : " (may need adjustment)");
  return 0;
}

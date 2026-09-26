// probe/cgroupv2.c — are we on a v2 hierarchy with delegation?
// Build: cc -o /tmp/probe_cgroup probe/cgroupv2.c && /tmp/probe_cgroup
#include <stdio.h>
#include <string.h>

int main(void) {
  FILE* f = fopen("/proc/self/cgroup", "r");
  if (f == NULL) {
    printf("cgroup: unknown (no /proc/self/cgroup)\n");
    return 1;
  }
  char line[512];
  int v2 = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, "0::", 3) == 0) {
      v2 = 1;
    }
  }
  fclose(f);
  FILE* c = fopen("/sys/fs/cgroup/cgroup.controllers", "r");
  if (v2 && c != NULL) {
    char ctrls[512] = {};
    if (fgets(ctrls, sizeof(ctrls), c) == NULL) {
      ctrls[0] = '\0';
    }
    fclose(c);
    printf("cgroup: v2 delegated? controllers: %s", ctrls);
    return 0;
  }
  if (c != NULL) {
    fclose(c);
  }
  printf("cgroup: %s\n", v2 ? "v2 (no controllers visible)" : "v1 or hybrid (prlimit fallback)");
  return v2 ? 0 : 1;
}

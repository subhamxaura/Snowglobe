// probe/syscall_info.c — do PTRACE_SYSCALL stops arrive as SIGTRAP|0x80 and does
// PTRACE_GET_SYSCALL_INFO report ENTRY/EXIT on this kernel?
// Build: cc -o /tmp/probe_syscall probe/syscall_info.c && /tmp/probe_syscall
// Expected: "stop sig=133" lines with op=1 (ENTRY, UAPI enum starts at NONE=0)
// alternating with op=2 (EXIT). A plain "stop sig=5" is the exec-stop.
// NOTE: an earlier tracer revision assumed ENTRY=0/EXIT=1 and decoded nothing;
// the UAPI enum is NONE=0, ENTRY=1, EXIT=2, SECCOMP=3 (<linux/ptrace.h>).
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

struct Sci {
  unsigned char op;
  unsigned char pad[3];
  unsigned int arch;
  unsigned long long ip, sp;
  struct {
    unsigned long long nr, a[6];
  } entry;
};

int main(void) {
  const pid_t c = fork();
  if (c == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);
    execl("/bin/true", "true", NULL);
    _exit(127);
  }
  int st = 0;
  waitpid(c, &st, 0);
  printf("first stop sig=%d\n", WSTOPSIG(st));
  ptrace(PTRACE_SETOPTIONS, c, 0, (void*)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
  ptrace(PTRACE_SYSCALL, c, 0, 0);
  for (int i = 0; i < 8; i++) {
    const pid_t p = waitpid(c, &st, 0);
    if (p < 0) {
      perror("waitpid");
      break;
    }
    if (WIFEXITED(st) || WIFSIGNALED(st)) {
      printf("exit\n");
      break;
    }
    struct Sci info = {0};
    const long r = ptrace(PTRACE_GET_SYSCALL_INFO, c, (void*)sizeof(info), &info);
    printf("stop sig=%d getinfo r=%ld op=%u nr=%llu\n", WSTOPSIG(st), r, info.op, info.entry.nr);
    ptrace(PTRACE_SYSCALL, c, 0, 0);
  }
  return 0;
}

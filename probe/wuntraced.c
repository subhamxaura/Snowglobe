// probe/wuntraced.c — untraced self-stops need WUNTRACED (Phase 4).
//
// The notify backend seizes its root after the child raises SIGSTOP to
// itself. The child is not traced yet at that point, and waitpid does NOT
// report untraced stops without WUNTRACED (man waitpid: "Status for
// traced children which have stopped is provided even if this option is
// not specified" — i.e. untraced stops need the flag). Without it the
// seize-phase wait hangs forever while the child sits in do_signal_stop
// (observed live on 6.6.87-WSL2). Post-seize every descendant is traced,
// so their stops report regardless.
//
// Build: cc -o /tmp/probe_wuntraced probe/wuntraced.c
// Usage: /tmp/probe_wuntraced <traceme 0|1> <wuntraced 0|1>
// Expect: hangs (kill it) unless traceme=1 or wuntraced=1.
// Prints are unbuffered so a killed run still shows how far it got.
#include <signal.h>
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
  int traceme = argc > 1 ? argv[1][0] - '0' : 0;
  int wuntraced = argc > 2 ? argv[2][0] - '0' : 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  pid_t c = fork();
  if (c < 0) {
    perror("fork");
    return 1;
  }
  if (c == 0) {
    if (traceme) {
      if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) {
        _exit(99);
      }
    }
    raise(SIGSTOP);
    _exit(42);
  }
  printf("parent: child=%d traceme=%d wuntraced=%d\n", (int)c, traceme, wuntraced);
  int status = 0;
  int opts = 0x40000000 /* __WALL */ | (wuntraced ? 0x00000002 /* WUNTRACED */ : 0);
  pid_t w = waitpid(c, &status, opts);
  printf("waitpid -> %d stopped=%d stopsig=%d\n", (int)w, WIFSTOPPED(status), WSTOPSIG(status));
  return 0;
}

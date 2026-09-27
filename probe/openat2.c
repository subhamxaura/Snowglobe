// probe/openat2.c — prove openat2 flag decoding reads struct open_how.
// Calls raw syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof how) with
// O_WRONLY|O_CREAT|O_TRUNC in how.flags. The tracer must read flags from the
// struct (offset 0), NOT from the arg register (which holds the pointer).
// Build: cc -o /tmp/probe_openat2 probe/openat2.c
// Run under snowglobe: expect fs.open write:true create:true trunc:true.
// Prints "openat2: ok" and exits 0 on success.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_openat2
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_openat2 437
#endif
#endif

// Minimal struct open_how (linux/openat2.h): flags, mode, resolve.
struct OpenHow {
  unsigned long long flags;
  unsigned long long mode;
  unsigned long long resolve;
};

int main(void) {
#ifdef SYS_openat2
  struct OpenHow how;
  memset(&how, 0, sizeof(how));
  how.flags = O_WRONLY | O_CREAT | O_TRUNC;
  how.mode = 0644;
  const long fd = syscall(SYS_openat2, AT_FDCWD, "/tmp/sg-openat2-probe", &how, sizeof(how));
  if (fd < 0) {
    printf("openat2: no: %s\n", strerror(errno));
    return 1;
  }
  const char* msg = "x";
  (void)!write((int)fd, msg, 1);
  close((int)fd);
  unlink("/tmp/sg-openat2-probe");
  printf("openat2: ok\n");
  return 0;
#else
  printf("openat2: no: no SYS_openat2 for this arch\n");
  return 1;
#endif
}

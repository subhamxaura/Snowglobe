// probe: does this kernel dispatch x32 syscalls (CONFIG_X86_X32_ABI)?
// An x32 getpid (nr | __X32_SYSCALL_BIT) issued from a normal 64-bit
// process succeeds iff the x32 ABI is enabled; ENOSYS otherwise. If
// enabled, seccomp nr-JEQ deny rules are bypassable via the x32 number
// (same AUDIT_ARCH_X86_64, bit-30-set nr misses every JEQ -> ALLOW),
// so the isolate filter must deny the bit (R5).
// Repro: gcc probe/x32.c -o /tmp/x32p && /tmp/x32p
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef __X32_SYSCALL_BIT
#define __X32_SYSCALL_BIT 0x40000000
#endif
#ifndef __NR_getpid
#define __NR_getpid 39
#endif
int main(void) {
  errno = 0;
  long r = syscall((long)(__NR_getpid | __X32_SYSCALL_BIT));
  if (r > 0) {
    printf("x32 ENABLED (x32 getpid -> %ld)\n", r);
    return 0;
  }
  printf("x32 DISABLED (x32 getpid errno %d %s)\n", errno, strerror(errno));
  return 1;
}

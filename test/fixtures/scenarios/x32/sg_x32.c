// sg_x32.c — one x32-ABI mkdirat (see test/fixtures/scenarios/README.md).
// Usage: sg_x32 <dir>. Issues a raw mkdirat with the x32 high bit set
// (nr | 0x40000000, R5 class) for <dir>/x32dir.
//
// Why mkdirat and not openat: mkdir events carry NO outcome keys in
// either backend, so the golden is immune to how the kernel dispatches
// the high-bit nr — natively (success, observed on kernels without a
// strict x32 ABI) or as true x32 (EFAULT on the truncated pointer where
// CONFIG_X86_X32_ABI=y). An openat golden would pin ok/errno and flake
// across those kernels. What both dispatches share is the ENTRY with
// the high-bit nr — and that is exactly what the backends must
// classify: pre-strip, ptrace saw Kind::None (no event); post-strip,
// both backends record fs.mkdir identically. Exit 0 either way.
// Non-x86_64 builds fall back to the native nr (goldens only run on
// x86_64; this keeps the helper compiling everywhere, e.g. the aarch64
// build-only job).
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: sg_x32 <dir>\n");
    return 2;
  }
  char path[1152];
  snprintf(path, sizeof(path), "%s/x32dir", argv[1]);
#ifdef __x86_64__
  const long nr = (long)((unsigned long)SYS_mkdirat | 0x40000000UL);
#else
  const long nr = (long)SYS_mkdirat;
#endif
  // dirfd + path + mode: same arg layout natively and under x32.
  (void)syscall(nr, (long)AT_FDCWD, path, (long)0755);
  printf("x32: done\n");
  return 0;
}

// probe/isolate_smoke.c — Block-1 isolate dance: parent-mapped userns,
// overlay, pivot_root, fresh proc, PID-1 reaping, cross-ns ptrace.
// FINDINGS (WSL2 6.6.87, uid 1000 — read before reusing this dance):
//  - self uid_map write: EPERM. Parent single-id map works
//    (see probe/map_parent.c); the middle must request maps over a pipe.
//  - overlay lowerdir=/: EINVAL, dmesg "failed to clone lowerpath".
//    lowerdir=/home (clean ext4): OK. lowerdir=/usr: EINVAL (poisoned by
//    nested overlay + 9p submounts). So lowerdir=/ wholesale is impossible
//    here; core/isolate uses selective overlays (repo, /etc) + ro binds
//    per the constitution §1.4 (project overlay + host / ro + tmpfs /tmp).
// Build: cc -o /tmp/izsmoke probe/isolate_smoke.c && /tmp/izsmoke
// Exit 0 + ALL-PASS lines, or FIRST-FAIL step with errno. Root-owned
// artifacts are never created (upper/work under /tmp, cleaned up).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/ptrace.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x00000008
#endif
#ifndef PTRACE_O_TRACEEXEC
#define PTRACE_O_TRACEEXEC 0x00000010
#endif
#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL 0x00100000
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420f
#endif

static int fails = 0;
#define CHECK(c, msg)                                                                              \
  do {                                                                                             \
    if (!(c)) {                                                                                    \
      printf("FAIL: %s: %s\n", msg, strerror(errno));                                              \
      fails = 1;                                                                                   \
      goto done;                                                                                   \
    } else {                                                                                       \
      printf("ok: %s\n", msg);                                                                     \
    }                                                                                              \
  } while (0)

static int pput(const char* path, const char* s) {
  int fd = open(path, O_WRONLY);
  if (fd < 0)
    return -1;
  size_t n = strlen(s), w = 0;
  while (w < n) {
    ssize_t k = write(fd, s + w, n - w);
    if (k < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      return -1;
    }
    w += (size_t)k;
  }
  close(fd);
  return 0;
}

// PID-1 init-helper: fork the agent, reap everything, exit with its code.
static void initHelper(void) {
  pid_t a = fork();
  if (a < 0)
    _exit(40);
  if (a == 0) {
    char* av[] = {(char*)"sh", (char*)"-c", (char*)"echo iz-ok > /etc/iz-probe-from-ns", NULL};
    execvp("sh", av);
    _exit(127);
  }
  int st = 0, code = 99;
  for (;;) {
    pid_t w = waitpid(-1, &st, 0);
    if (w < 0)
      break;
    if (w == a)
      code = WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st);
  }
  _exit(code);
}

static void middle(const char* base, int reqFd, int ackFd) {
  setvbuf(stdout, NULL, _IONBF, 0); // _exit() below never flushes: unbuffered
  char upper[512], work[512], merged[512], oldr[512];
  snprintf(upper, sizeof upper, "%s/upper", base);
  snprintf(work, sizeof work, "%s/work", base);
  snprintf(merged, sizeof merged, "%s/merged", base);
  snprintf(oldr, sizeof oldr, "%s/merged/oldroot", base);
  errno = 0;
  if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    printf("FAIL: unshare U+NS: %s\n", strerror(errno));
    _exit(10);
  }
  // Parent-driven mapping (self uid_map is EPERM on this kernel — see
  // probe/map_parent.c): request, then wait for the ack byte.
  if (write(reqFd, "M", 1) != 1) {
    printf("FAIL: map request\n");
    _exit(10);
  }
  {
    char ack = 0;
    ssize_t nr = 0;
    while (nr == 0) {
      nr = read(ackFd, &ack, 1);
      if (nr < 0 && errno != EINTR) {
        printf("FAIL: map ack: %s\n", strerror(errno));
        _exit(11);
      }
    }
    if (ack != 'G') {
      printf("FAIL: map refused\n");
      _exit(11);
    }
  }
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
    printf("FAIL: rprivate\n");
    _exit(12);
  }
  if (mount("tmpfs", base, "tmpfs", 0, "size=64m,mode=0755") != 0) {
    printf("FAIL: tmpfs base\n");
    _exit(13);
  }
  if (mkdir(upper, 0755) || mkdir(work, 0755) || mkdir(merged, 0755) || mkdir(oldr, 0755)) {
    printf("FAIL: mkdir layers\n");
    _exit(14);
  }
  char opts[2048], procd[512], tmpd[512];
  snprintf(opts, sizeof opts, "lowerdir=/,upperdir=%s,workdir=%s", upper, work);
  if (mount("overlay", merged, "overlay", 0, opts) != 0) {
    printf("FAIL: overlay lowerdir=/: %s\n", strerror(errno));
    _exit(15);
  }
  snprintf(procd, sizeof procd, "%s/proc", merged);
  if (mount("proc", procd, "proc", 0, "") != 0) {
    printf("FAIL: proc\n");
    _exit(16);
  }
  snprintf(tmpd, sizeof tmpd, "%s/tmp", merged);
  if (mount("tmpfs", tmpd, "tmpfs", 0, "size=32m,mode=1777") != 0) {
    printf("FAIL: tmpfs /tmp\n");
    _exit(17);
  }
  if (syscall(SYS_pivot_root, merged, oldr) != 0) {
    printf("FAIL: pivot_root\n");
    _exit(18);
  }
  if (chdir("/") != 0) {
    printf("FAIL: chdir /\n");
    _exit(19);
  }
  if (umount2("/oldroot", MNT_DETACH) != 0) {
    printf("FAIL: detach oldroot\n");
    _exit(20);
  }
  if (unshare(CLONE_NEWPID) != 0) {
    printf("FAIL: unshare PID\n");
    _exit(21);
  }
  if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) {
    printf("FAIL: TRACEME\n");
    _exit(22);
  }
  raise(SIGSTOP);
  pid_t init = fork();
  if (init < 0) {
    _exit(23);
  }
  if (init == 0) {
    initHelper();
    _exit(99);
  }
  int st = 0, code = 98;
  for (;;) {
    pid_t w = waitpid(-1, &st, 0);
    if (w < 0)
      break;
    if (w == init)
      code = WIFEXITED(st) ? WEXITSTATUS(st) : 90 + WTERMSIG(st);
  }
  _exit(code);
}

int main(void) {
  char base[] = "/tmp/izsmoke-XXXXXX";
  CHECK(mkdtemp(base) != NULL, "mkdtemp base");
  int toC[2], toP[2]; // map-request pipe (middle→parent) + ack pipe
  CHECK(pipe(toC) == 0 && pipe(toP) == 0, "map pipes");
  pid_t m = fork();
  CHECK(m >= 0, "fork middle");
  if (m == 0) {
    close(toC[1]);
    close(toP[0]);
    middle(base, toP[1], toC[0]);
    _exit(127);
  }
  close(toC[0]);
  close(toP[1]);
  // Parent-driven id map (single-id self-map, the unshare --map-user rule).
  {
    char req = 0;
    ssize_t nr = 0;
    while (nr == 0) {
      nr = read(toP[0], &req, 1);
      if (nr < 0 && errno != EINTR)
        break;
    }
    CHECK(nr == 1 && req == 'M', "middle requested maps");
    char pgm[64], ump[64], gmp[64], umap[64], gmap[64];
    snprintf(pgm, sizeof pgm, "/proc/%d/setgroups", (int)m);
    snprintf(ump, sizeof ump, "/proc/%d/uid_map", (int)m);
    snprintf(gmp, sizeof gmp, "/proc/%d/gid_map", (int)m);
    snprintf(umap, sizeof umap, "0 %d 1", (int)getuid());
    snprintf(gmap, sizeof gmap, "0 %d 1", (int)getgid());
    pput(pgm, "deny");
    int ok = (pput(ump, umap) == 0) && (pput(gmp, gmap) == 0);
    CHECK(ok, "parent wrote uid/gid maps");
    CHECK(write(toC[1], "G", 1) == 1, "parent acked maps");
  }

  // Tracer side (outside every namespace): first stop, then follow clones.
  int st = 0;
  pid_t w = waitpid(m, &st, 0);
  CHECK(w == m && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "middle first SIGSTOP");
  long opts = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD;
  CHECK(ptrace(PTRACE_SETOPTIONS, m, NULL, (void*)opts) == 0, "tracer SETOPTIONS");
  CHECK(ptrace(PTRACE_CONT, m, NULL, NULL) == 0, "tracer CONT middle");

  int grandchild = -1, sawSyscall = 0, middleCode = -1;
  for (int i = 0; i < 2000; i++) {
    w = waitpid(-1, &st, 0);
    if (w < 0 && errno == ECHILD)
      break;
    CHECK(w >= 0, "tracer waitpid");
    if (WIFEXITED(w) || WIFSIGNALED(w)) {
      if (w == m)
        middleCode = WIFEXITED(w) ? WEXITSTATUS(w) : 200;
      continue;
    }
    if (!WIFSTOPPED(w))
      continue;
    int sig = WSTOPSIG(st);
    unsigned ev = (unsigned)((st >> 16) & 0xffff);
    if (ev == PTRACE_EVENT_CLONE) {
      unsigned long msg = 0;
      CHECK(ptrace(PTRACE_GETEVENTMSG, w, NULL, &msg) == 0, "tracer GETEVENTMSG");
      if ((pid_t)msg != m)
        grandchild = (int)msg;
      CHECK(ptrace(PTRACE_CONT, w, NULL, NULL) == 0, "tracer CONT clone");
      continue;
    }
    if (sig == (SIGTRAP | 0x80)) {
      // Syscall stop on some tracee: prove GET_SYSCALL_INFO works, incl.
      // across the pid-ns boundary (the errno question from the spec).
      struct ptrace_syscall_info info;
      memset(&info, 0, sizeof info);
      errno = 0;
      long r = ptrace(PTRACE_GET_SYSCALL_INFO, w, (void*)sizeof info, &info);
      if (r >= 0)
        sawSyscall = 1;
      else if (grandchild < 0 || w == grandchild) {
        printf("note: GET_SYSCALL_INFO on grandchild: %s\n", strerror(errno));
      }
      CHECK(ptrace(PTRACE_SYSCALL, w, NULL, NULL) == 0, "tracer SYSCALL cont");
      continue;
    }
    int deliver = (sig == SIGSTOP || sig == SIGTRAP) ? 0 : sig;
    CHECK(ptrace(PTRACE_CONT, w, NULL, (void*)(long)deliver) == 0, "tracer CONT");
  }
  CHECK(grandchild > 0, "observed grandchild clone across pidns");
  CHECK(sawSyscall, "GET_SYSCALL_INFO decoded a cross-ns syscall");
  CHECK(middleCode == 0, "exit code propagated middle<-init<-agent");

  // Persistence + host-untouched checks from outside.
  char up[512], host[512];
  snprintf(up, sizeof up, "%s/upper/etc/iz-probe-from-ns", base);
  int fd = open(up, O_RDONLY);
  CHECK(fd >= 0, "upper copy-up exists");
  char buf[32] = {0};
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  CHECK(n > 0 && strncmp(buf, "iz-ok", 5) == 0, "upper content intact");
  snprintf(host, sizeof host, "/etc/iz-probe-from-ns");
  CHECK(access(host, F_OK) != 0, "host /etc untouched");
done:
  // Best-effort cleanup (mounts die with the namespaces).
  if (!fails) {
    char cmd[640];
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    (void)!system(cmd);
  }
  if (!fails)
    printf("ALL-PASS isolate_smoke\n");
  return fails ? 1 : 0;
}

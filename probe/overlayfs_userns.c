// probe/overlayfs_userns.c — can an UNPRIVILEGED process mount overlayfs
// inside a user+mount namespace? Full dance, no guessing: the child calls
// unshare(CLONE_NEWUSER|CLONE_NEWNS), maps ITSELF via /proc/self/{uid_map,
// gid_map} (the util-linux `unshare --map-user` pattern — no parent
// coordination needed), then mounts tmpfs + overlay, proves the merged view
// stacks lowerdir (seed file), and unmounts.
// Build: cc -o /tmp/probe_overlay probe/overlayfs_userns.c && /tmp/probe_overlay
// When started as root it forks first and the tester child drops privs
// CLEANLY (fixed gid, empty supplementary groups, restored dumpability) —
// setuid(2) in-process is NOT enough: it retains root's groups and clears
// dumpability, and then even /proc/self/uid_map opens EACCES. So "yes" always
// proves the unprivileged path Phase 2 needs.
// Finding (WSL2 6.6): even /proc/self/setgroups is EACCES for nobody, and a
// same-uid parent cannot write the child's uid_map either — so the probe
// self-maps (uid_map required, gid_map best-effort) and lets the mount decide.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_DUMPABLE
#define PR_SET_DUMPABLE 4
#endif
#ifndef PR_GET_DUMPABLE
#define PR_GET_DUMPABLE 3
#endif

// Best-effort whole-file write. Returns 0 on success, -1 with errno set.
static int putFileFlags(const char* path, const char* content, int flags) {
  const int fd = open(path, flags, 0644);
  if (fd < 0) {
    return -1;
  }
  const size_t len = strlen(content);
  size_t done = 0;
  while (done < len) {
    const ssize_t n = write(fd, content + done, len - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return -1;
    }
    done += (size_t)n;
  }
  close(fd);
  return 0;
}

// Regular files (create/truncate). /proc map files go through procPutFile.
static int putFile(const char* path, const char* content) {
  return putFileFlags(path, content, O_WRONLY | O_CREAT | O_TRUNC);
}

// /proc/pid/{setgroups,uid_map,gid_map}: plain O_WRONLY (no CREATE/TRUNC).
static int procPutFile(const char* path, const char* content) {
  return putFileFlags(path, content, O_WRONLY);
}

// Child side: errFd takes a NUL-free reason string on failure, then _exit(1).
static void childFail(int errFd, const char* step) {
  char msg[256];
  snprintf(msg, sizeof(msg), "%s: %s", step, strerror(errno));
  (void)!write(errFd, msg, strlen(msg));
  _exit(1);
}

static void runChild(int errFd, const char* base) {
  char lower[256], upper[256], work[256], merged[256];
  char seed[512], hello[512];
  snprintf(lower, sizeof(lower), "%s/lower", base);
  snprintf(upper, sizeof(upper), "%s/upper", base);
  snprintf(work, sizeof(work), "%s/work", base);
  snprintf(merged, sizeof(merged), "%s/merged", base);
  snprintf(seed, sizeof(seed), "%s/seed", lower);
  snprintf(hello, sizeof(hello), "%s/hello", merged);

  if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    childFail(errFd, "unshare(NEWUSER|NEWNS)");
  }
  // Self-map: write our own uid_map/gid_map (setgroups first where allowed).
  // A same-uid parent cannot write these for us (EACCES), so do it here.
  char map[64];
  char notes[256] = {0};
  snprintf(map, sizeof(map), "0 %d 1", (int)getuid());
  if (procPutFile("/proc/self/setgroups", "deny") != 0 && errno != ENOENT) {
    snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes), "; setgroups unwritable (%s)",
             strerror(errno));
  }
  if (procPutFile("/proc/self/uid_map", map) != 0) {
    childFail(errFd, "self uid_map");
  }
  snprintf(map, sizeof(map), "0 %d 1", (int)getgid());
  if (procPutFile("/proc/self/gid_map", map) != 0) {
    snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes), "; gid_map unwritten (%s)",
             strerror(errno));
  }
  // Stash notes for the parent: the err pipe carries "OK:<notes>" on success.
  {
    char out[300];
    snprintf(out, sizeof(out), "OK:%s", notes);
    (void)!write(errFd, out, strlen(out));
  }
  // Do not propagate our mounts back to the parent namespace.
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
    childFail(errFd, "make-rprivate");
  }
  if (mount("tmpfs", base, "tmpfs", 0, "size=64m,mode=0755") != 0) {
    childFail(errFd, "mount tmpfs");
  }
  if (mkdir(lower, 0755) != 0 || mkdir(upper, 0755) != 0 || mkdir(work, 0755) != 0 ||
      mkdir(merged, 0755) != 0) {
    childFail(errFd, "mkdir layers");
  }
  if (putFile(seed, "lower-seed") != 0) {
    childFail(errFd, "seed lower");
  }
  char opts[1024];
  snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s", lower, upper, work);
  if (mount("overlay", merged, "overlay", 0, opts) != 0) {
    childFail(errFd, "mount overlay");
  }
  // The merged view must show lowerdir content (proves stacking, not a plain dir).
  char check[512], buf[32] = {0};
  snprintf(check, sizeof(check), "%s/seed", merged);
  const int fd = open(check, O_RDONLY);
  if (fd < 0 || read(fd, buf, sizeof(buf) - 1) < 0) {
    if (fd >= 0) {
      close(fd);
    }
    childFail(errFd, "read merged/seed");
  }
  close(fd);
  if (strcmp(buf, "lower-seed") != 0) {
    childFail(errFd, "merged content mismatch");
  }
  if (putFile(hello, "upper-write") != 0) {
    childFail(errFd, "write merged/hello");
  }
  if (umount(merged) != 0) {
    childFail(errFd, "umount merged");
  }
  if (umount(base) != 0) {
    childFail(errFd, "umount base");
  }
  _exit(0);
}

// Drop root cleanly in a forked tester child: fixed gid, NO supplementary
// groups (setuid(2) alone retains root's group list!), fixed uid, then
// restore dumpability (setuid clears it; a non-dumpable process cannot even
// open its own /proc/self/uid_map — ptrace_may_access denies it).
// _exit(2) with a message when any step fails.
static void dropClean(void) {
  const struct passwd* pw = getpwnam("nobody");
  if (pw == NULL) {
    printf("overlayfs-in-userns: inconclusive: no nobody user\n");
    _exit(2);
  }
  if (setgid(pw->pw_gid) != 0 || setgroups(0, NULL) != 0 || setuid(pw->pw_uid) != 0) {
    printf("overlayfs-in-userns: inconclusive: cannot drop privs: %s\n", strerror(errno));
    _exit(2);
  }
  (void)!prctl(PR_SET_DUMPABLE, 1);
  if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1) {
    printf("overlayfs-in-userns: inconclusive: dumpable not restored\n");
    _exit(2);
  }
  if (geteuid() == 0 || getegid() == 0) {
    printf("overlayfs-in-userns: inconclusive: still root after drop\n");
    _exit(2);
  }
}

static int runTester(void) {
  char base[] = "/tmp/sg-overlay-XXXXXX";
  if (mkdtemp(base) == NULL) {
    printf("overlayfs-in-userns: no: mkdtemp: %s\n", strerror(errno));
    return 1;
  }
  int errFds[2];
  if (pipe(errFds) != 0) {
    printf("overlayfs-in-userns: no: pipe: %s\n", strerror(errno));
    return 1;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    printf("overlayfs-in-userns: no: fork: %s\n", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    close(errFds[0]);
    runChild(errFds[1], base);
    _exit(127); // unreachable
  }
  close(errFds[1]);

  int status = 0;
  waitpid(pid, &status, 0);
  // The child always writes one message: "OK:<notes>" or "<reason>".
  char msg[384] = {0};
  size_t got = 0;
  for (;;) {
    const ssize_t n = read(errFds[0], msg + got, sizeof(msg) - 1 - got);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break; // EOF: child exited, all write ends closed
    }
    got += (size_t)n;
    if (got >= sizeof(msg) - 1) {
      break;
    }
  }
  msg[got] = '\0';
  // Best-effort cleanup of the base dir (child already unmounted).
  rmdir(base);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && strncmp(msg, "OK:", 3) == 0) {
    printf("overlayfs-in-userns: yes%s\n", msg + 3);
    return 0;
  }
  if (msg[0] != '\0' && strncmp(msg, "OK:", 3) != 0) {
    printf("overlayfs-in-userns: no: %s\n", msg);
  } else {
    printf("overlayfs-in-userns: no: child status %d\n", status);
  }
  return 1;
}

int main(void) {
  if (geteuid() == 0) {
    // Fork first: the tester lineage must never have been root (setuid(2)
    // in-process retains groups and clears dumpability — both fatal here).
    const pid_t t = fork();
    if (t < 0) {
      printf("overlayfs-in-userns: no: fork: %s\n", strerror(errno));
      return 1;
    }
    if (t == 0) {
      dropClean(); // _exit(2) with message on failure
      return runTester();
    }
    int st = 0;
    waitpid(t, &st, 0);
    if (WIFEXITED(st)) {
      return WEXITSTATUS(st);
    }
    printf("overlayfs-in-userns: no: tester killed by signal %d\n", WTERMSIG(st));
    return 1;
  }
  return runTester();
}

#include "doctor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "../isolate/landlock.hpp"
#include "../isolate/seccomp.hpp"

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

// UAPI constants from <linux/landlock.h> / <linux/seccomp.h>. Values are
// pinned at compile time by probe/landlock.c and probe/seccomp_notif.c,
// which use the real headers; manual defines here keep doctor musl-friendly.
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif
#if !defined(SYS_landlock_create_ruleset) && (defined(__x86_64__) || defined(__aarch64__))
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SECCOMP_GET_ACTION_AVAIL
#define SECCOMP_GET_ACTION_AVAIL 2
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#if !defined(SYS_seccomp) && (defined(__x86_64__) || defined(__aarch64__))
#define SYS_seccomp 317
#endif
#ifndef PR_SET_DUMPABLE
#define PR_SET_DUMPABLE 4
#endif
#ifndef PR_GET_DUMPABLE
#define PR_GET_DUMPABLE 3
#endif
#endif

namespace snowglobe::doctor {
namespace {

std::string readFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) {
    return "";
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool commandExists(const std::string& cmd) {
#ifdef __linux__
  // PATH lookup without popen: try access() on each component.
  const char* path = std::getenv("PATH");
  if (path == nullptr) {
    return false;
  }
  std::string p(path);
  std::size_t i = 0;
  while (i <= p.size()) {
    const std::size_t j = p.find(':', i);
    const std::string dir = p.substr(i, j == std::string::npos ? j : j - i);
    if (!dir.empty() && ::access((dir + "/" + cmd).c_str(), X_OK) == 0) {
      return true;
    }
    if (j == std::string::npos) {
      break;
    }
    i = j + 1;
  }
  return false;
#else
  (void)cmd;
  return false;
#endif
}

} // namespace

#ifdef __linux__
namespace {

// All helpers below run on Linux only.

std::string errnoText(int e) {
  const char* m = std::strerror(e);
  return m != nullptr ? std::string(m) : ("errno " + std::to_string(e));
}

std::string trimWs(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    return "";
  }
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Real Landlock query: returns the highest supported ABI, or -errno.
long landlockAbi() {
#ifdef SYS_landlock_create_ruleset
  errno = 0;
  const long abi =
      syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  return abi >= 0 ? abi : -errno;
#else
  return -ENOSYS;
#endif
}

Capability checkLandlock() {
  const long abi = landlockAbi();
  if (abi >= 0) {
    return {"landlock", true, "abi " + std::to_string(abi) + " (see probe/landlock.c)"};
  }
  if (abi == -ENOSYS) {
    return {"landlock", false, "kernel compiled without Landlock (ENOSYS)"};
  }
  if (abi == -EOPNOTSUPP) {
    return {"landlock", false, "Landlock LSM not enabled (EOPNOTSUPP)"};
  }
  return {"landlock", false, std::string("unavailable: ") + errnoText((int)-abi)};
}

// Real seccomp query: 0 means SECCOMP_RET_USER_NOTIF is available.
long seccompNotifAvail() {
#ifdef SYS_seccomp
  unsigned int act = SECCOMP_RET_USER_NOTIF;
  errno = 0;
  const long r = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
  return r == 0 ? 0 : -errno;
#else
  return -ENOSYS;
#endif
}

Capability checkSeccompNotif() {
  const long r = seccompNotifAvail();
  if (r == 0) {
    return {"seccomp-notify", true, "SECCOMP_RET_USER_NOTIF available (see probe/seccomp_notif.c)"};
  }
  if (r == -EINVAL) {
    return {"seccomp-notify", false, "action unknown: kernel < 5.11"};
  }
  if (r == -ENOSYS) {
    return {"seccomp-notify", false, "no seccomp(2)"};
  }
  return {"seccomp-notify", false, std::string("unavailable: ") + errnoText((int)-r)};
}

// Notify-backend usability (ADR-0009, Phase 4 Block 3): same kernel
// capability as seccomp-notify, phrased as the feature it gates
// (`run --backend=notify`). Honest yes/no; --isolate + notify stays
// unsupported regardless (tracer-level 69, not a kernel lack).
Capability checkNotifyBackend() {
  const long r = seccompNotifAvail();
  if (r == 0) {
    return {"notify-backend", true, "--backend=notify usable (user-notify available)"};
  }
  if (r == -EINVAL) {
    return {"notify-backend", false, "--backend=notify unavailable: kernel < 5.11"};
  }
  if (r == -ENOSYS) {
    return {"notify-backend", false, "--backend=notify unavailable: no seccomp(2)"};
  }
  return {"notify-backend", false,
          std::string("--backend=notify unavailable: ") + errnoText((int)-r)};
}

// Write whole file with plain O_WRONLY (right for /proc map files).
bool procPut(const std::string& path, const std::string& content) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    return false;
  }
  size_t done = 0;
  while (done < content.size()) {
    const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return false;
    }
    done += static_cast<size_t>(n);
  }
  ::close(fd);
  return true;
}

bool putFileCreate(const std::string& path, const std::string& content) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return false;
  }
  size_t done = 0;
  while (done < content.size()) {
    const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return false;
    }
    done += static_cast<size_t>(n);
  }
  ::close(fd);
  return true;
}

// Tester child for the overlayfs-in-userns check: unshare, PARENT-WRITTEN
// id maps (self uid_map is EPERM on some kernels — probe/map_parent.c),
// mount tmpfs+overlay, verify the merged view, unmount. Writes "OK:<notes>"
// (or a reason) to errFd, then _exit()s. Mirrors probe/overlayfs_userns.c.
// mapReqW/mapAckR are the parent handshake (request "M", wait for "G").
void overlayTesterChild(int errFd, const char* base, int mapReqW, int mapAckR) {
  auto fail = [&](const char* step) {
    const int e = errno;
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s: %s", step, errnoText(e).c_str());
    (void)!::write(errFd, msg, std::strlen(msg));
    _exit(1);
  };
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    fail("unshare(NEWUSER|NEWNS)");
  }
  char notes[256] = {0};
  {
    char req = 'M';
    if (::write(mapReqW, &req, 1) != 1) {
      fail("map request");
    }
    char ack = 0;
    ssize_t n = 0;
    do {
      n = ::read(mapAckR, &ack, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1 || ack != 'G') {
      errno = EPERM;
      fail("idmap refused");
    }
  }
  if (::getuid() != 0) {
    errno = EPERM;
    fail("idmap ineffective");
  }
  {
    char out[300];
    std::snprintf(out, sizeof(out), "OK:%s", notes);
    (void)!::write(errFd, out, std::strlen(out));
  }
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    fail("make-rprivate");
  }
  if (::mount("tmpfs", base, "tmpfs", 0, "size=64m,mode=0755") != 0) {
    fail("mount tmpfs");
  }
  char lower[512], upper[512], work[512], merged[512];
  std::snprintf(lower, sizeof(lower), "%s/lower", base);
  std::snprintf(upper, sizeof(upper), "%s/upper", base);
  std::snprintf(work, sizeof(work), "%s/work", base);
  std::snprintf(merged, sizeof(merged), "%s/merged", base);
  if (::mkdir(lower, 0755) != 0 || ::mkdir(upper, 0755) != 0 || ::mkdir(work, 0755) != 0 ||
      ::mkdir(merged, 0755) != 0) {
    fail("mkdir layers");
  }
  char seed[1024], check[1024], hello[1024], opts[2048];
  std::snprintf(seed, sizeof(seed), "%s/seed", lower);
  std::snprintf(check, sizeof(check), "%s/seed", merged);
  std::snprintf(hello, sizeof(hello), "%s/hello", merged);
  std::snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s", lower, upper, work);
  if (!putFileCreate(seed, "lower-seed")) {
    fail("seed lower");
  }
  if (::mount("overlay", merged, "overlay", 0, opts) != 0) {
    fail("mount overlay");
  }
  const int fd = ::open(check, O_RDONLY);
  char buf[32] = {0};
  if (fd < 0 || ::read(fd, buf, sizeof(buf) - 1) < 0) {
    if (fd >= 0) {
      ::close(fd);
    }
    fail("read merged/seed");
  }
  ::close(fd);
  if (std::string(buf) != "lower-seed") {
    fail("merged content mismatch");
  }
  if (!putFileCreate(hello, "upper-write")) {
    fail("write merged/hello");
  }
  if (::umount(merged) != 0) {
    fail("umount merged");
  }
  if (::umount(base) != 0) {
    fail("umount base");
  }
  ::rmdir(base); // best-effort; base lives on the parent fs
  _exit(0);
}

// Drop root cleanly for the tester lineage (see probe/overlayfs_userns.c:
// in-process setuid retains groups and clears dumpability — both fatal).
// Reports through errFd, then the caller continues as an unprivileged user.
void dropToNobody(int errFd) {
  auto fail = [&](const char* step) {
    const int e = errno;
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s: %s", step, errnoText(e).c_str());
    (void)!::write(errFd, msg, std::strlen(msg));
    _exit(2);
  };
  const struct passwd* pw = ::getpwnam("nobody");
  if (pw == nullptr) {
    fail("no nobody user");
  }
  if (::setgid(pw->pw_gid) != 0 || ::setgroups(0, nullptr) != 0 || ::setuid(pw->pw_uid) != 0) {
    fail("drop privs");
  }
  (void)!::prctl(PR_SET_DUMPABLE, 1);
  if (::prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1) {
    fail("dumpable not restored");
  }
}

// Parent-written single-id maps for the tester child (the unshare
// --map-user rule): setgroups-deny first, then uid_map + gid_map.
// Self-mapping is EPERM on some kernels (probe/map_parent.c).
bool writeMapsFor(pid_t child) {
  char path[64];
  char map[64];
  std::snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)child);
  procPut(path, "deny"); // best-effort: gid_map reports its own errno
  std::snprintf(map, sizeof(map), "0 %d 1", (int)::getuid());
  std::snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)child);
  if (!procPut(path, map)) {
    return false;
  }
  std::snprintf(map, sizeof(map), "0 %d 1", (int)::getgid());
  std::snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)child);
  return procPut(path, map);
}

Capability checkOverlayUserns() {
  char base[] = "/tmp/sg-doctor-XXXXXX";
  int errFds[2] = {-1, -1};
  int mapReq[2] = {-1, -1}; // tester->doctor "M"
  int mapAck[2] = {-1, -1}; // doctor->tester "G"/"F"
  if (::pipe(errFds) != 0) {
    return {"overlayfs-in-userns", false, std::string("pipe: ") + errnoText(errno)};
  }
  if (::pipe(mapReq) != 0 || ::pipe(mapAck) != 0) {
    return {"overlayfs-in-userns", false, std::string("map pipe: ") + errnoText(errno)};
  }
  const pid_t tester = ::fork();
  if (tester < 0) {
    return {"overlayfs-in-userns", false, std::string("fork: ") + errnoText(errno)};
  }
  if (tester == 0) {
    ::close(errFds[0]);
    ::close(mapReq[0]);
    ::close(mapAck[1]);
    if (::geteuid() == 0) {
      dropToNobody(errFds[1]); // _exit(2) on failure
    }
    if (::mkdtemp(base) == nullptr) {
      const int e = errno;
      char msg[256];
      std::snprintf(msg, sizeof(msg), "mkdtemp: %s", errnoText(e).c_str());
      (void)!::write(errFds[1], msg, std::strlen(msg));
      _exit(1);
    }
    overlayTesterChild(errFds[1], base, mapReq[1], mapAck[0]); // never returns
    _exit(127);
  }
  ::close(errFds[1]);
  ::close(mapReq[1]);
  ::close(mapAck[0]);
  // Parent-written id maps (self uid_map is EPERM on some kernels).
  {
    char req = 0;
    ssize_t n = 0;
    do {
      n = ::read(mapReq[0], &req, 1);
    } while (n < 0 && errno == EINTR);
    ::close(mapReq[0]);
    const char ack = (n == 1 && req == 'M' && writeMapsFor(tester)) ? 'G' : 'F';
    (void)!::write(mapAck[1], &ack, 1);
    ::close(mapAck[1]);
    if (ack != 'G') {
      int status = 0;
      ::waitpid(tester, &status, 0);
      return {"overlayfs-in-userns", false, "idmap refused (see probe/map_parent.c)"};
    }
  }
  int status = 0;
  ::waitpid(tester, &status, 0);
  char msg[384] = {0};
  size_t got = 0;
  for (;;) {
    const ssize_t n = ::read(errFds[0], msg + got, sizeof(msg) - 1 - got);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0 || got >= sizeof(msg) - 1) {
      break;
    }
    got += static_cast<size_t>(n);
  }
  ::close(errFds[0]);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && std::strncmp(msg, "OK:", 3) == 0) {
    std::string detail =
        "mounted tmpfs+overlay, verified merged view (see probe/overlayfs_userns.c)";
    if (msg[3] != '\0') {
      detail += msg + 3;
    }
    return {"overlayfs-in-userns", true, detail};
  }
  if (msg[0] != '\0' && std::strncmp(msg, "OK:", 3) != 0) {
    return {"overlayfs-in-userns", false, msg};
  }
  return {"overlayfs-in-userns", false, "tester failed"};
}

} // namespace
#endif

std::vector<Capability> checkAll() {
  std::vector<Capability> out;
#ifdef __linux__
  struct utsname u = {};
  uname(&u);
  out.push_back({"kernel", true, std::string(u.sysname) + " " + u.release + " " + u.machine});

  const std::string usernsCtl = trimWs(readFile("/proc/sys/kernel/unprivileged_userns_clone"));
  const bool hasUsernsFile = ::access("/proc/self/ns/user", F_OK) == 0;
  // Ubuntu's AppArmor knob: 0 = allow, 1 = restrict unprivileged userns. Absent elsewhere.
  const std::string aa = trimWs(readFile("/proc/sys/kernel/apparmor_restrict_unprivileged_userns"));
  std::string usernsDetail = usernsCtl.empty()
                                 ? (hasUsernsFile ? "present" : "no /proc/self/ns/user")
                                 : "unprivileged_userns_clone=" + usernsCtl;
  usernsDetail += aa.empty() ? "; apparmor_restrict_unprivileged_userns: absent"
                             : "; apparmor_restrict_unprivileged_userns=" + aa;
  out.push_back({"user-namespace", hasUsernsFile, usernsDetail});

  out.push_back(checkOverlayUserns());
  out.push_back(checkLandlock());
  out.push_back(checkSeccompNotif());
  out.push_back(checkNotifyBackend());

  const std::string cgroup = readFile("/proc/self/cgroup");
  out.push_back({"cgroup-v2", cgroup.find("0::") != std::string::npos,
                 cgroup.find("0::") != std::string::npos ? "v2 hierarchy"
                                                         : "cgroup: no (prlimit fallback active)"});

  const bool pasta = commandExists("pasta");
  const bool slirp = commandExists("slirp4netns");
  out.push_back({"pasta/slirp4netns", pasta || slirp,
                 pasta ? "pasta found" : (slirp ? "slirp4netns found" : "neither on PATH")});

  const std::string yama = readFile("/proc/sys/kernel/yama/ptrace_scope");
  out.push_back({"ptrace", true, yama.empty() ? "available" : ("yama ptrace_scope=" + yama)});
#else
  out.push_back({"kernel", false, "non-Linux dev box — Linux-first per AGENTS.md §1.1; see CI"});
  out.push_back({"user-namespace", false, "requires Linux"});
  out.push_back({"overlayfs", false, "requires Linux"});
  out.push_back({"landlock", false, "requires Linux"});
  out.push_back({"seccomp-notify", false, "requires Linux >= 5.11"});
  out.push_back({"notify-backend", false, "requires Linux >= 5.11"});
  out.push_back({"cgroup-v2", false, "requires Linux"});
  out.push_back({"pasta/slirp4netns", false, "requires Linux user-mode networking"});
  out.push_back({"ptrace", false, "requires Linux"});
#endif
  return out;
}

int printTable() {
  const auto caps = checkAll();
  std::printf("%-20s %-6s %s\n", "capability", "status", "detail");
  for (const auto& c : caps) {
    std::printf("%-20s %-6s %s\n", c.name.c_str(), c.available ? "yes" : "no", c.detail.c_str());
  }
  return 0;
}

#ifdef __linux__
namespace {

// Tester outcomes via _exit codes (parent decodes, never trusts text).
constexpr int kProbeOk = 0;

// userns row: unshare + parent-written single-id map (the isolate rule).
bool probeUserns(std::string& detail) {
  int toC[2] = {-1, -1}, toP[2] = {-1, -1};
  if (::pipe(toC) != 0 || ::pipe(toP) != 0) {
    detail = std::string("pipe: ") + errnoText(errno);
    return false;
  }
  const pid_t c = ::fork();
  if (c < 0) {
    detail = std::string("fork: ") + errnoText(errno);
    return false;
  }
  if (c == 0) {
    ::close(toC[1]);
    ::close(toP[0]);
    if (::unshare(CLONE_NEWUSER) != 0) {
      _exit(10);
    }
    if (::write(toP[1], "M", 1) != 1) {
      _exit(11);
    }
    char ack = 0;
    if (::read(toC[0], &ack, 1) != 1 || ack != 'G') {
      _exit(12);
    }
    _exit(::getuid() == 0 ? kProbeOk : 13);
  }
  ::close(toC[0]);
  ::close(toP[1]);
  char req = 0;
  bool mapped = false;
  if (::read(toP[0], &req, 1) == 1 && req == 'M') {
    mapped = writeMapsFor(c);
    const char ack = mapped ? 'G' : 'F';
    (void)!::write(toC[1], &ack, 1);
  }
  ::close(toP[0]);
  ::close(toC[1]);
  int st = 0;
  ::waitpid(c, &st, 0);
  if (!mapped) {
    detail = "parent idmap refused";
    return false;
  }
  if (WIFEXITED(st) && WEXITSTATUS(st) == kProbeOk) {
    detail = "unshare + parent single-id map";
    return true;
  }
  detail = "tester exit " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return false;
}

// pidns + procfs rows: unshare USER (parent-mapped) + PID, fork; the
// PID-1 child reports getpid()==1 and attempts a fresh proc mount (the
// attempt outcome IS the procfs row: EPERM here → tmpfs fallback).
bool probePidns(bool& procfs, std::string& detail, std::string& procDetail) {
  procfs = false;
  int toC[2] = {-1, -1}, toP[2] = {-1, -1};
  int rep[2] = {-1, -1}; // child report bytes: '1' pidns-ok, 'P' proc-ok
  if (::pipe(toC) != 0 || ::pipe(toP) != 0 || ::pipe(rep) != 0) {
    detail = std::string("pipe: ") + errnoText(errno);
    return false;
  }
  const pid_t c = ::fork();
  if (c < 0) {
    detail = std::string("fork: ") + errnoText(errno);
    return false;
  }
  if (c == 0) {
    ::close(toC[1]);
    ::close(toP[0]);
    ::close(rep[0]);
    if (::unshare(CLONE_NEWUSER) != 0) {
      _exit(10);
    }
    if (::write(toP[1], "M", 1) != 1) {
      _exit(11);
    }
    char ack = 0;
    if (::read(toC[0], &ack, 1) != 1 || ack != 'G') {
      _exit(12);
    }
    if (::unshare(CLONE_NEWPID) != 0) {
      _exit(13);
    }
    const pid_t init = ::fork();
    if (init < 0) {
      _exit(14);
    }
    if (init == 0) {
      char r = (::getpid() == 1) ? '1' : '0';
      (void)!::write(rep[1], &r, 1);
      char dir[] = "/tmp/sg-doctor-proc-XXXXXX";
      if (::mkdtemp(dir) == nullptr) {
        _exit(15);
      }
      if (::mount("proc", dir, "proc", 0, "") == 0) {
        const char p = 'P';
        (void)!::write(rep[1], &p, 1);
        ::umount(dir);
      }
      ::rmdir(dir);
      _exit(kProbeOk);
    }
    int st = 0;
    ::waitpid(init, &st, 0);
    _exit(WIFEXITED(st) && WEXITSTATUS(st) == kProbeOk ? kProbeOk : 16);
  }
  ::close(toC[0]);
  ::close(toP[1]);
  ::close(rep[1]);
  char req = 0;
  bool mapped = false;
  if (::read(toP[0], &req, 1) == 1 && req == 'M') {
    mapped = writeMapsFor(c);
    const char ack = mapped ? 'G' : 'F';
    (void)!::write(toC[1], &ack, 1);
  }
  ::close(toP[0]);
  ::close(toC[1]);
  bool sawOne = false;
  bool sawProc = false;
  char b = 0;
  while (::read(rep[0], &b, 1) == 1) {
    sawOne = sawOne || (b == '1');
    sawProc = sawProc || (b == 'P');
  }
  ::close(rep[0]);
  int st = 0;
  ::waitpid(c, &st, 0);
  const bool exited = WIFEXITED(st) && WEXITSTATUS(st) == kProbeOk && mapped;
  procfs = sawProc;
  procDetail = sawProc ? "fresh proc mount works"
                       : "optional \u2014 REQUIRED for Bun/Node-class runtimes (issue #4); "
                         "empty-tmpfs fallback active";
  if (!mapped) {
    detail = "parent idmap refused";
    return false;
  }
  if (!exited || !sawOne) {
    detail = "pid-1 fork/check failed";
    return false;
  }
  detail = "unshare + PID-1 fork verified";
  return true;
}

// seccomp row: install the REAL isolate filter in a throwaway child;
// io_uring_setup must die SIGSYS (non-vacuous: the kill fires with or
// without privilege). EPERM paths are proven under --isolate itself.
bool probeSeccomp(std::string& detail) {
  const pid_t c = ::fork();
  if (c < 0) {
    detail = std::string("fork: ") + errnoText(errno);
    return false;
  }
  if (c == 0) {
    std::string err;
    if (!isolate::installIsolateFilter(err)) {
      _exit(10);
    }
#ifdef SYS_io_uring_setup
    ::syscall(SYS_io_uring_setup, 8, nullptr);
    _exit(11); // must not survive the call
#else
    _exit(12);
#endif
  }
  int st = 0;
  ::waitpid(c, &st, 0);
  if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS) {
    detail = "isolate filter kills io_uring_setup (SIGSYS)";
    return true;
  }
  detail = "no SIGSYS death (exit/signal mismatch)";
  return false;
}

// landlock row: enforce a scratch RO/RW ruleset via the shared helper;
// the RO write must EPERM (non-vacuous: the dir is writable unconfined).
bool probeLandlock(std::string& detail) {
  char roT[] = "/tmp/sg-doctor-ll-ro-XXXXXX";
  char rwT[] = "/tmp/sg-doctor-ll-rw-XXXXXX";
  if (::mkdtemp(roT) == nullptr || ::mkdtemp(rwT) == nullptr) {
    detail = "mkdtemp scratch failed";
    return false;
  }
  const pid_t c = ::fork();
  if (c < 0) {
    detail = std::string("fork: ") + errnoText(errno);
    return false;
  }
  if (c == 0) {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      _exit(14);
    }
    std::string err;
    if (!isolate::enforceLandlock({rwT}, err)) {
      _exit(10);
    }
    const std::string ok = std::string(rwT) + "/f";
    const int fd = ::open(ok.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      _exit(11);
    }
    ::close(fd);
    const std::string no = std::string(roT) + "/f";
    errno = 0;
    const int fd2 = ::open(no.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd2 >= 0) {
      ::close(fd2);
      _exit(12);
    }
    _exit((errno == EPERM || errno == EACCES) ? 0 : 13);
  }
  int st = 0;
  ::waitpid(c, &st, 0);
  ::unlink((std::string(rwT) + "/f").c_str());
  ::rmdir(roT);
  ::rmdir(rwT);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
    detail = "scratch RO denied (EPERM), RW allowed";
    return true;
  }
  detail = "enforcement smoke exit " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  return false;
}

// cgroup-deleg row (best-effort by design): can we mkdir in cgroupfs?
bool probeCgroupDeleg(std::string& detail) {
  // mkdtemp needs a template it can mutate: build under a fixed parent.
  std::string cand = "/sys/fs/cgroup/sg-doctor-probe";
  if (::mkdir(cand.c_str(), 0755) != 0) {
    if (errno == EEXIST) {
      ::rmdir(cand.c_str());
      if (::mkdir(cand.c_str(), 0755) != 0) {
        detail = "cgroup: no (prlimit fallback active)";
        return false;
      }
    } else {
      detail = "cgroup: no (prlimit fallback active)";
      return false;
    }
  }
  ::rmdir(cand.c_str());
  detail = "delegated (limits will apply)";
  return true;
}

} // namespace
#endif

std::vector<IsolateRow> checkIsolate() {
  std::vector<IsolateRow> out;
#ifdef __linux__
  {
    std::string d;
    out.push_back({"userns", true, probeUserns(d), d});
  }
  {
    // mount+overlay reuses the table's real dance (parent-mapped).
    const Capability c = checkOverlayUserns();
    out.push_back({"mount+overlay", true, c.available, c.detail});
  }
  {
    std::string d;
    std::string pd;
    bool procfs = false;
    out.push_back({"pidns", true, probePidns(procfs, d, pd), d});
    out.push_back({"procfs", false, procfs, pd});
  }
  {
    std::string d;
    out.push_back({"seccomp", true, probeSeccomp(d), d});
  }
  {
    std::string d;
    out.push_back({"landlock", true, probeLandlock(d), d});
  }
  {
    std::string d;
    out.push_back({"cgroup-deleg", false, probeCgroupDeleg(d), d});
  }
  out.push_back({"network", false, true, "host network stays (no netns this phase)"});
#else
  out.push_back({"userns", true, false, "requires Linux"});
  out.push_back({"mount+overlay", true, false, "requires Linux"});
  out.push_back({"pidns", true, false, "requires Linux"});
  out.push_back({"procfs", false, false, "requires Linux"});
  out.push_back({"seccomp", true, false, "requires Linux"});
  out.push_back({"landlock", true, false, "requires Linux"});
  out.push_back({"cgroup-deleg", false, false, "requires Linux"});
  out.push_back({"network", false, true, "host network stays (no netns this phase)"});
#endif
  return out;
}

std::string isolateReport() {
  std::string s = "isolate readiness (required rows must be green):\n";
  char line[512];
  for (const IsolateRow& r : checkIsolate()) {
    std::snprintf(line, sizeof(line), "  %-14s %-4s %-9s %s\n", r.name.c_str(),
                  r.required ? "req" : "opt", r.ok ? "green" : "RED", r.detail.c_str());
    s += line;
  }
  return s;
}

int printIsolateTable() {
  const auto rows = checkIsolate();
  std::printf("%-14s %-4s %-6s %s\n", "isolate", "req", "status", "detail");
  bool ready = true;
  for (const auto& r : rows) {
    std::printf("%-14s %-4s %-6s %s\n", r.name.c_str(), r.required ? "req" : "opt",
                r.ok ? "green" : "RED", r.detail.c_str());
    if (r.required && !r.ok) {
      ready = false;
    }
  }
  return ready ? 0 : 69;
}

} // namespace snowglobe::doctor

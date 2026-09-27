#include "doctor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
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

// Tester child for the overlayfs-in-userns check: unshare, self-map, mount
// tmpfs+overlay, verify the merged view, unmount. Writes "OK:<notes>" (or a
// reason) to errFd, then _exit()s. Mirrors probe/overlayfs_userns.c.
void overlayTesterChild(int errFd, const char* base) {
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
  char map[64];
  char notes[256] = {0};
  std::snprintf(map, sizeof(map), "0 %d 1", (int)::getuid());
  if (!procPut("/proc/self/setgroups", "deny") && errno != ENOENT) {
    std::snprintf(notes + std::strlen(notes), sizeof(notes) - std::strlen(notes),
                  "; setgroups unwritable (%s)", errnoText(errno).c_str());
  }
  if (!procPut("/proc/self/uid_map", map)) {
    fail("self uid_map");
  }
  std::snprintf(map, sizeof(map), "0 %d 1", (int)::getgid());
  if (!procPut("/proc/self/gid_map", map)) {
    std::snprintf(notes + std::strlen(notes), sizeof(notes) - std::strlen(notes),
                  "; gid_map unwritten (%s)", errnoText(errno).c_str());
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

Capability checkOverlayUserns() {
  char base[] = "/tmp/sg-doctor-XXXXXX";
  int errFds[2] = {-1, -1};
  if (::pipe(errFds) != 0) {
    return {"overlayfs-in-userns", false, std::string("pipe: ") + errnoText(errno)};
  }
  const pid_t tester = ::fork();
  if (tester < 0) {
    return {"overlayfs-in-userns", false, std::string("fork: ") + errnoText(errno)};
  }
  if (tester == 0) {
    ::close(errFds[0]);
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
    overlayTesterChild(errFds[1], base); // never returns
    _exit(127);
  }
  ::close(errFds[1]);
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

  const std::string cgroup = readFile("/proc/self/cgroup");
  out.push_back({"cgroup-v2", cgroup.find("0::") != std::string::npos,
                 cgroup.find("0::") != std::string::npos ? "v2 hierarchy"
                                                         : "no 0:: entry (prlimit fallback)"});

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

} // namespace snowglobe::doctor

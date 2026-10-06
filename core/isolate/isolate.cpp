// Unprivileged isolation runner (design + probe evidence: ADR-0007).
// Containment of accidents, never a security boundary.
#include "isolate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace snowglobe::isolate {
namespace {

#ifdef __linux__
std::string errnoText(int e) {
  const char* m = std::strerror(e);
  return m != nullptr ? std::string(m) : ("errno " + std::to_string(e));
}

// Write the whole buffer (EINTR-safe). False on error.
bool writeAll(int fd, const char* data, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t n = ::write(fd, data + done, len - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

bool procPut(const std::string& path, const char* content, int& err) {
  err = 0;
  const int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    err = errno;
    return false;
  }
  const bool ok = writeAll(fd, content, std::strlen(content));
  err = ok ? 0 : errno;
  ::close(fd);
  return ok;
}

// Read one line (capped); false on EOF/error.
bool readLine(int fd, std::string& out, std::string& why) {
  out.clear();
  char c = 0;
  for (size_t i = 0; i < 512; ++i) {
    ssize_t n = 0;
    do {
      n = ::read(fd, &c, 1);
    } while (n < 0 && errno == EINTR);
    if (n == 0) {
      why = "EOF";
      return false;
    }
    if (n < 0) {
      why = errnoText(errno);
      return false;
    }
    if (c == '\n') {
      return true;
    }
    out.push_back(c);
  }
  why = "line too long";
  return false;
}

// mkdir -p (single level is not enough: overlay/upper needs overlay/ first).
bool mkpath(const std::string& path, std::string& error) {
  std::string cur;
  const bool abs = !path.empty() && path[0] == '/';
  size_t i = 0;
  if (abs) {
    cur = "/";
    i = 1;
  }
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) {
      j = path.size();
    }
    const std::string comp = path.substr(i, j - i);
    if (!comp.empty()) {
      cur += (cur.size() > 1 ? "/" : "") + comp;
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        error = "mkdir " + cur + ": " + errnoText(errno);
        return false;
      }
    }
    i = j + 1;
  }
  return true;
}

// Ancestors of an absolute path, shallowest first, excluding "/" and the
// path itself: /a/b/c -> [/a, /a/b].
std::vector<std::string> ancestors(const std::string& abs) {
  std::vector<std::string> out;
  std::string cur;
  const std::string rest = abs.size() > 1 ? abs.substr(1) : "";
  size_t i = 0;
  while (i < rest.size()) {
    const size_t j = rest.find('/', i);
    const std::string comp = rest.substr(i, j == std::string::npos ? j : j - i);
    cur += "/" + comp;
    if (j == std::string::npos) {
      break;
    }
    out.push_back(cur);
    i = j + 1;
  }
  return out;
}

// Parse super options (mountinfo field 6) into MS_* keep-flags. A remount
// that drops nosuid/nodev/noexec reads as an escalation and fails EPERM
// in a userns — the flags must ride along with BIND|REMOUNT|RDONLY.
unsigned long keepFlags(const std::string& opts) {
  unsigned long f = 0;
  size_t i = 0;
  while (i <= opts.size()) {
    size_t j = opts.find(',', i);
    if (j == std::string::npos) {
      j = opts.size();
    }
    const std::string o = opts.substr(i, j - i);
    if (o == "nosuid") {
      f |= MS_NOSUID;
    } else if (o == "nodev") {
      f |= MS_NODEV;
    } else if (o == "noexec") {
      f |= MS_NOEXEC;
    } else if (o == "sync") {
      f |= MS_SYNCHRONOUS;
    } else if (o == "dirsync") {
      f |= MS_DIRSYNC;
    } else if (o == "relatime") {
      f |= MS_RELATIME;
    } else if (o == "strictatime") {
      f |= MS_STRICTATIME;
    } else if (o == "noatime") {
      f |= MS_NOATIME;
    } else if (o == "nodiratime") {
      f |= MS_NODIRATIME;
    }
    i = j + 1;
  }
  return f;
}
// Remount every mount at/below dst read-only, deepest first (parsed from
// /proc/self/mountinfo: field 5 = mount point, field 6 = super options).
// Plain top-only remount leaves submounts writable — a containment hole
// for e.g. /usr/lib/wsl. The top mount MUST succeed (false); submount
// failures are tolerated best-effort (the top ro stands).
bool remountRoRecursive(const std::string& dst) {
  std::vector<std::pair<std::string, std::string>> mps; // (point, opts)
  FILE* f = ::fopen("/proc/self/mountinfo", "r");
  if (f == nullptr) {
    return false; // cannot verify: refuse rather than claim ro
  }
  char* line = nullptr;
  size_t cap = 0;
  ssize_t len = 0;
  while ((len = ::getline(&line, &cap, f)) >= 0) {
    // fields: ... - fstype source opts; field 5 (0-based 4) is the point.
    int field = 0;
    std::string mp;
    std::string sup;
    std::string cur;
    for (ssize_t k = 0; k < len; ++k) {
      if (line[k] == ' ' || line[k] == '\n') {
        if (field == 4) {
          mp = cur;
        } else if (field == 5) {
          sup = cur;
        }
        ++field;
        cur.clear();
      } else {
        cur.push_back(line[k]);
      }
    }
    if (!mp.empty() &&
        (mp == dst || (mp.size() > dst.size() && mp.compare(0, dst.size(), dst) == 0 &&
                       mp[dst.size()] == '/'))) {
      mps.emplace_back(mp, sup);
    }
  }
  ::free(line);
  ::fclose(f);
  std::sort(mps.begin(), mps.end(),
            [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  bool topOk = false;
  int topErr = 0;
  bool sawTop = false;
  for (const auto& [mp, sup] : mps) {
    const unsigned long flags =
        static_cast<unsigned long>(MS_BIND | MS_REMOUNT | MS_RDONLY) | keepFlags(sup);
    if (::mount(nullptr, mp.c_str(), nullptr, flags, nullptr) == 0) {
      if (mp == dst) {
        topOk = true;
      }
    } else if (mp == dst && !sawTop) {
      topErr = errno;
      sawTop = true;
    }
  }
  if (!topOk) {
    errno = (topErr != 0) ? topErr : (sawTop ? EINVAL : ENOENT);
    return false;
  }
  return true;
}

bool bindRo(const std::string& src, const std::string& dst, std::string& error) {
  if (::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
    error = "bind " + src + ": " + errnoText(errno);
    return false;
  }
  if (!remountRoRecursive(dst)) {
    error = "bind " + src + ": remount ro refused: " + errnoText(errno);
    return false;
  }
  return true;
}

bool mountOverlay(const std::string& lower, const std::string& upper, const std::string& work,
                  const std::string& dst, std::string& error, const char* what) {
  const std::string opts = "lowerdir=" + lower + ",upperdir=" + upper + ",workdir=" + work;
  if (::mount("overlay", dst.c_str(), "overlay", 0, opts.c_str()) != 0) {
    error = std::string("mount overlay ") + what + ": " + errnoText(errno);
    return false;
  }
  return true;
}

void initHelper(const std::vector<std::string>& cmd) {
  std::vector<char*> cargv;
  cargv.reserve(cmd.size() + 1);
  for (const auto& a : cmd) {
    cargv.push_back(const_cast<char*>(a.c_str()));
  }
  cargv.push_back(nullptr);
  const pid_t agent = ::fork();
  if (agent < 0) {
    _exit(71);
  }
  if (agent == 0) {
    ::execvp(cargv[0], cargv.data());
    _exit(127);
  }
  int st = 0;
  int code = 99;
  for (;;) {
    const pid_t w = ::waitpid(-1, &st, 0);
    if (w < 0) {
      break;
    }
    if (w == agent) {
      code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
  }
  _exit(code);
}

} // namespace

#endif

bool prepareRunDir(const std::string& runDir, OverlayDirs& dirs, std::string& error) {
#ifdef __linux__
  const std::string base = runDir + "/overlay";
  dirs.upper = base + "/upper";
  dirs.work = base + "/work";
  dirs.etcUpper = base + "/etc-upper";
  dirs.etcWork = base + "/etc-work";
  dirs.mnt = base + "/mnt";
  for (const std::string* d : {&dirs.upper, &dirs.work, &dirs.etcUpper, &dirs.etcWork, &dirs.mnt}) {
    if (!mkpath(*d, error)) {
      error = "isolate: " + error;
      return false;
    }
  }
  // Workdirs must start empty for overlayfs (stale --out reuse fails loud).
  for (const std::string* d : {&dirs.work, &dirs.etcWork}) {
    DIR* dp = ::opendir(d->c_str());
    if (dp == nullptr) {
      error = "isolate: opendir " + *d + ": " + errnoText(errno);
      return false;
    }
    bool empty = true;
    while (const struct dirent* de = ::readdir(dp)) {
      const std::string n = de->d_name;
      if (n != "." && n != "..") {
        empty = false;
        break;
      }
    }
    ::closedir(dp);
    if (!empty) {
      error = "isolate: workdir not empty (stale --out?): " + *d;
      return false;
    }
  }
  return true;
#else
  (void)runDir;
  (void)dirs;
  error = "isolate: requires Linux";
  return false;
#endif
}

bool makePipes(ChildPipes& p, std::string& error) {
#ifdef __linux__
  int a[2] = {-1, -1}, b[2] = {-1, -1}, c[2] = {-1, -1};
  if (::pipe2(a, O_CLOEXEC) != 0 || ::pipe2(b, O_CLOEXEC) != 0 || ::pipe2(c, O_CLOEXEC) != 0) {
    error = std::string("isolate: pipe: ") + errnoText(errno);
    return false;
  }
  p.mapReqR = a[0];
  p.mapReqW = a[1];
  p.mapAckR = b[0];
  p.mapAckW = b[1];
  p.statusR = c[0];
  p.statusW = c[1];
  return true;
#else
  (void)p;
  error = "isolate: requires Linux";
  return false;
#endif
}

void closeSupervisorEnds(ChildPipes& p) {
#ifdef __linux__
  for (int fd : {p.mapReqR, p.mapAckW, p.statusR}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  p.mapReqR = p.mapAckW = p.statusR = -1;
#else
  (void)p;
#endif
}

void closeMiddleEnds(ChildPipes& p) {
#ifdef __linux__
  for (int fd : {p.mapReqW, p.mapAckR, p.statusW}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  p.mapReqW = p.mapAckR = p.statusW = -1;
#else
  (void)p;
#endif
}

bool serveMaps(pid_t child, ChildPipes& p, std::string& error) {
#ifdef __linux__
  char req = 0;
  ssize_t n = 0;
  do {
    n = ::read(p.mapReqR, &req, 1);
  } while (n < 0 && errno == EINTR);
  if (n != 1 || req != 'M') {
    error = "isolate: child died before map request";
    return false;
  }
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)child);
  int se = 0;
  if (!procPut(path, "deny", se) && se != ENOENT) {
    // Unwritable setgroups (non-ENOENT) fails gid_map below with its own
    // errno; keep going so the error names the map, not the knob.
  }
  char map[64];
  std::snprintf(map, sizeof(map), "0 %d 1", (int)::getuid());
  std::snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)child);
  int ue = 0;
  if (!procPut(path, map, ue)) {
    const std::string msg = "F";
    (void)!::write(p.mapAckW, msg.data(), 1);
    error = "isolate: uid_map: " + errnoText(ue);
    return false;
  }
  std::snprintf(map, sizeof(map), "0 %d 1", (int)::getgid());
  std::snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)child);
  int ge = 0;
  if (!procPut(path, map, ge)) {
    const std::string msg = "F";
    (void)!::write(p.mapAckW, msg.data(), 1);
    error = "isolate: gid_map: " + errnoText(ge);
    return false;
  }
  if (!writeAll(p.mapAckW, "G", 1)) {
    error = std::string("isolate: map ack: ") + errnoText(errno);
    return false;
  }
  return true;
#else
  (void)child;
  (void)p;
  error = "isolate: requires Linux";
  return false;
#endif
}

bool awaitReady(pid_t child, ChildPipes& p, std::string& error) {
#ifdef __linux__
  std::string line;
  std::string why;
  if (!readLine(p.statusR, line, why)) {
    error = "isolate: child died during setup (" + why + ")";
    int st = 0;
    ::waitpid(child, &st, 0);
    return false;
  }
  if (line == "ok") {
    closeMiddleEnds(p);
    return true;
  }
  if (line.compare(0, 4, "ERR ") == 0) {
    error = line.substr(4);
  } else {
    error = "isolate: bad setup status";
  }
  int st = 0;
  ::waitpid(child, &st, 0);
  return false;
#else
  (void)child;
  (void)p;
  error = "isolate: requires Linux";
  return false;
#endif
}

void enterChild(const ChildConfig& cfg) {
#ifdef __linux__
  const int statusW = cfg.statusW;
  // errno is read at call time (argument evaluation precedes any string
  // building inside the body), so call sites pass it straight from the
  // failing call — no syscall may intervene, not even closedir (the
  // process _exits here, fds die with it).
  auto failE = [&](const std::string& step, int e) {
    const std::string msg = "ERR isolate: " + step + ": " + errnoText(e) + "\n";
    (void)!::write(statusW, msg.data(), msg.size());
    _exit(70);
  };
  auto fail = [&](const std::string& step) { failE(step, errno); };
  auto failMsg = [&](const std::string& full) {
    const std::string msg = "ERR isolate: " + full + "\n";
    (void)!::write(statusW, msg.data(), msg.size());
    _exit(70);
  };
  char cwd[4096] = {};
  if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
    cwd[0] = '\0';
  }
  const std::string repo = cfg.projectDir;
  const std::string merged = cfg.dirs.mnt;

  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    fail("unshare userns");
  }
  if (!writeAll(cfg.mapReqW, "M", 1)) {
    fail("map request");
  }
  {
    char ack = 0;
    ssize_t n = 0;
    do {
      n = ::read(cfg.mapAckR, &ack, 1);
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
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    fail("make-rprivate");
  }
  if (::mount("tmpfs", cfg.dirs.mnt.c_str(), "tmpfs", 0, "size=256m,mode=0755") != 0) {
    fail("mount tmpfs base");
  }
  // The new root IS the tmpfs base (a mountpoint, pivotable).
  const std::string oldroot = merged + "/oldroot";
  if (::mkdir(merged.c_str(), 0755) != 0 && errno != EEXIST) {
    fail("mkdir merged");
  }
  if (::mkdir(oldroot.c_str(), 0755) != 0 && errno != EEXIST) {
    fail("mkdir oldroot");
  }

  // Ancestors of the repo (and /etc when the repo nests under it) are
  // created empty; everything else top-level is symlinked or ro-bound.
  std::vector<std::string> anc = ancestors(repo);
  const bool repoUnderEtc = repo == "/etc" || (repo.size() > 5 && repo.compare(0, 5, "/etc/") == 0);
  const auto isAnc = [&](const std::string& hostPath) {
    for (const auto& a : anc) {
      if (a == hostPath) {
        return true;
      }
    }
    return false;
  };
  std::string walkErr;
  DIR* dp = ::opendir("/");
  if (dp == nullptr) {
    fail("opendir /");
  }
  struct dirent* de = nullptr;
  while ((de = ::readdir(dp)) != nullptr) {
    const std::string name = de->d_name;
    if (name == "." || name == ".." || name == "proc" || name == "sys" || name == "dev" ||
        name == "run" || name == "tmp") {
      continue;
    }
    const std::string src = "/" + name;
    const std::string dst = merged + src;
    struct stat st = {};
    if (::lstat(src.c_str(), &st) != 0) {
      continue; // raced away; mounts below tolerate gaps
    }
    if (S_ISLNK(st.st_mode)) {
      char tgt[4096] = {};
      const ssize_t n = ::readlink(src.c_str(), tgt, sizeof(tgt) - 1);
      if (n > 0) {
        tgt[n] = '\0';
        ::symlink(tgt, dst.c_str());
      }
      continue;
    }
    if (!S_ISDIR(st.st_mode)) {
      continue; // stray top-level files (none on FHS roots)
    }
    if (::mkdir(dst.c_str(), 0755) != 0 && errno != EEXIST) {
      fail("mkdir " + dst);
    }
    if (name == "etc" || isAnc(src) || src == repo) {
      continue; // overlay or nested-overwrite target, mounted below
    }
    if (!bindRo(src, dst, walkErr)) {
      failMsg("bind " + src + " (" + walkErr + ")");
    }
  }
  ::closedir(dp);

  // Specials (before the repo chain: merged/tmp is a fresh tmpfs that the
  // chain builds on — mounting it earlier would shadow a repo under /tmp).
  for (const char* d : {"proc", "sys", "dev", "run", "tmp"}) {
    const std::string p = merged + "/" + d;
    if (::mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) {
      fail("mkdir " + p);
    }
  }
  // /proc is an EMPTY tmpfs here, not a fresh procfs: this kernel denies
  // new proc/sysfs instances inside a userns (EPERM, tmpfs mounts work).
  // Host pids stay invisible (test d passes vacuously); agents that need
  // /proc introspection degrade — recorded in ADR-0007 + limitations.
  if (::mount("tmpfs", (merged + "/proc").c_str(), "tmpfs", 0, "size=64m,mode=0555") != 0) {
    fail("mount tmpfs proc");
  }
  if (!bindRo("/sys", merged + "/sys", walkErr)) {
    failMsg("bind /sys (" + walkErr + ")");
  }
  if (::mount("/dev", (merged + "/dev").c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
    fail("bind /dev");
  }
  if (::mount("tmpfs", (merged + "/run").c_str(), "tmpfs", 0, "size=64m,mode=0755") != 0) {
    fail("mount tmpfs run");
  }
  if (::mount("tmpfs", (merged + "/tmp").c_str(), "tmpfs", 0, "size=256m,mode=1777") != 0) {
    fail("mount tmpfs tmp");
  }

  // Overlays last (deepest mount wins for nested repo-under-/etc).
  // The repo chain is built here — after the specials tmpfs mounts, so a
  // repo under /tmp lands on the fresh tmpfs instead of being shadowed.
  {
    std::string cur = merged;
    std::string rest = repo.size() > 1 ? repo.substr(1) : "";
    size_t i = 0;
    while (i < rest.size()) {
      const size_t j = rest.find('/', i);
      cur += "/" + rest.substr(i, j == std::string::npos ? j : j - i);
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        fail("mkdir chain " + cur);
      }
      if (j == std::string::npos) {
        break;
      }
      i = j + 1;
    }
  }
  if (!repoUnderEtc && repo != "/etc") {
    if (!mountOverlay("/etc", cfg.dirs.etcUpper, cfg.dirs.etcWork, merged + "/etc", walkErr,
                      "/etc")) {
      failMsg(walkErr);
    }
  }
  if (!mountOverlay(repo, cfg.dirs.upper, cfg.dirs.work, merged + repo, walkErr, "repo")) {
    failMsg(walkErr);
  }

  if (::syscall(SYS_pivot_root, merged.c_str(), oldroot.c_str()) != 0) {
    fail("pivot_root");
  }
  if (::chdir("/") != 0) {
    fail("chdir /");
  }
  if (::umount2("/oldroot", MNT_DETACH) != 0) {
    fail("detach oldroot");
  }
  if (cwd[0] != '\0' && ::chdir(cwd) != 0) {
    if (::chdir("/") != 0) {
      fail("chdir fallback");
    }
  }
  if (::unshare(CLONE_NEWPID) != 0) {
    fail("unshare pidns");
  }
  if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) {
    fail("traceme");
  }
  {
    const char ok[] = "ok\n";
    (void)!::write(statusW, ok, sizeof(ok) - 1);
  }
  ::raise(SIGSTOP);
  const pid_t init = ::fork();
  if (init < 0) {
    _exit(72); // post-handshake fork failure: tracer sees exit, reports it
  }
  if (init == 0) {
    initHelper(cfg.cmd);
  }
  int st = 0;
  int code = 98;
  for (;;) {
    const pid_t w = ::waitpid(-1, &st, 0);
    if (w < 0) {
      break;
    }
    if (w == init) {
      code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
  }
  _exit(code);
#else
  (void)cfg;
  _exit(69);
#endif
}

} // namespace snowglobe::isolate

// Unprivileged isolation runner (design + probe evidence: ADR-0007).
// Containment of accidents, never a security boundary.
#include "isolate.hpp"
#include "landlock.hpp"
#include "seccomp.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Global environ (like cli/main.cpp: <unistd.h> may not expose it under
// -Wpedantic). At global scope — never inside a namespace.
extern char** environ;

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

} // namespace

bool isSecretName(const std::string& name) {
  static const char* kSuffixes[] = {"KEY", "TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL"};
  std::string up = name;
  for (char& c : up) {
    c = static_cast<char>(std::toupper((unsigned char)c));
  }
  for (const char* s : kSuffixes) {
    const size_t n = std::strlen(s);
    if (up.size() >= n && up.compare(up.size() - n, n, s) == 0) {
      return true;
    }
  }
  return false;
}
void stripSecretEnv(const std::vector<std::string>& allowEnv) {
#ifdef __linux__
  if (environ == nullptr) {
    return;
  }
  // Snapshot names first: unsetenv while iterating environ is unsafe.
  std::vector<std::string> names;
  for (char** e = environ; *e != nullptr; ++e) {
    const std::string entry(*e);
    const size_t eq = entry.find('=');
    names.push_back(eq == std::string::npos ? entry : entry.substr(0, eq));
  }
  for (const std::string& name : names) {
    if (!isSecretName(name)) {
      continue;
    }
    bool keep = false;
    for (const std::string& a : allowEnv) {
      if (a == name) {
        keep = true;
        break;
      }
    }
    if (!keep) {
      const std::string base = name;
      const size_t us = base.rfind("_BASE_URL");
      if (us != std::string::npos && us + 9 == base.size()) {
        keep = true; // proxy wiring always passes (ADR-0005/ADR-0007)
      }
    }
    if (!keep) {
      ::unsetenv(name.c_str());
    }
  }
#else
  (void)allowEnv; // non-Linux: --isolate is unreachable (CLI gates it)
#endif
}

std::vector<std::string> defaultSecretMasks(const std::string& homeDir,
                                            const std::vector<std::string>& allowPath) {
  std::vector<std::string> cands;
  if (!homeDir.empty() && homeDir[0] == '/') {
    cands.push_back(homeDir + "/.ssh");
    cands.push_back(homeDir + "/.aws");
    cands.push_back(homeDir + "/.gnupg");
  }
  std::vector<std::string> out;
  for (const std::string& m : cands) {
    bool exempt = false;
    for (const std::string& a : allowPath) {
      if (a == m) {
        exempt = true;
        break;
      }
      // Parent exemption: --allow-path $HOME (or any parent) exempts children.
      if (m.size() > a.size() && m.compare(0, a.size(), a) == 0 && m[a.size()] == '/' &&
          (a.size() == 1 || true)) {
        // a == "/" would exempt everything (project=/ is already rejected);
        // still honor it literally (m starts with "/").
        exempt = true;
        break;
      }
    }
    if (!exempt) {
      out.push_back(m);
    }
  }
  return out;
}

bool parseMemSize(const std::string& s, long long& bytes, std::string& error) {
  if (s == "max") {
    bytes = -1;
    return true;
  }
  size_t n = 0;
  while (n < s.size() && s[n] >= '0' && s[n] <= '9') {
    ++n;
  }
  if (n == 0) {
    error = "bad size '" + s + "' (bytes or K/M/G, or max)";
    return false;
  }
  long long v = 0;
  try {
    v = std::stoll(s.substr(0, n));
  } catch (...) {
    error = "bad size '" + s + "'";
    return false;
  }
  const std::string suf = s.substr(n);
  long long mult = 1;
  if (suf.empty()) {
    mult = 1;
  } else if (suf == "K" || suf == "k") {
    mult = 1024LL;
  } else if (suf == "M" || suf == "m") {
    mult = 1024LL * 1024;
  } else if (suf == "G" || suf == "g") {
    mult = 1024LL * 1024 * 1024;
  } else {
    error = "bad size suffix in '" + s + "' (K/M/G)";
    return false;
  }
  if (v < 0 || (mult > 1 && v > (LLONG_MAX / mult))) {
    error = "bad size '" + s + "'";
    return false;
  }
  bytes = v * mult;
  return true;
}

bool joinCgroup(const std::string& tag, int childPid, long long memBytes, long long pidsMax,
                std::string& note, std::string& outPath) {
#ifdef __linux__
  outPath.clear();
  // cgroup v2 only (0:: entry); anything else is best-effort "no".
  bool v2 = false;
  std::string ownScope;
  {
    FILE* f = ::fopen("/proc/self/cgroup", "r");
    if (f == nullptr) {
      note = "no /proc/self/cgroup";
      return false;
    }
    char* line = nullptr;
    size_t cap = 0;
    while (::getline(&line, &cap, f) >= 0) {
      const std::string l = line;
      if (l.compare(0, 4, "0::/") == 0) {
        v2 = true;
        ownScope = l.substr(3);
        while (!ownScope.empty() && (ownScope.back() == '\n' || ownScope.back() == '\r')) {
          ownScope.pop_back();
        }
      }
    }
    ::free(line);
    ::fclose(f);
  }
  if (!v2) {
    note = "no cgroup v2 hierarchy";
    return false;
  }
  // Candidate parents: our own scope dir first (delegation point), then
  // the v2 root. First writable win; all denied → honest note, no fail.
  std::vector<std::string> parents = {"/sys/fs/cgroup" + ownScope, "/sys/fs/cgroup"};
  for (const std::string& parent : parents) {
    const std::string dir = parent + "/sg-" + tag;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
      continue;
    }
    // Best-effort controller enable (needs parent write; ignore failure —
    // the limit writes below are the real test).
    {
      const std::string sub = parent + "/cgroup.subtree_control";
      const int fd = ::open(sub.c_str(), O_WRONLY);
      if (fd >= 0) {
        (void)!::write(fd, "+memory +pids", 14);
        ::close(fd);
      }
    }
    bool ok = true;
    int werr = 0;
    if (memBytes >= 0) {
      ok = procPut(dir + "/memory.max", std::to_string(memBytes).c_str(), werr) && ok;
    }
    if (pidsMax >= 0) {
      ok = procPut(dir + "/pids.max", std::to_string(pidsMax).c_str(), werr) && ok;
    }
    char pidbuf[32] = {};
    std::snprintf(pidbuf, sizeof(pidbuf), "%d", childPid);
    ok = procPut(dir + "/cgroup.procs", pidbuf, werr) && ok;
    if (ok) {
      outPath = dir;
      note = "limits on " + dir;
      return true;
    }
    ::rmdir(dir.c_str());
  }
  note = "cgroup delegation denied; continuing without limits";
  return false;
#else
  (void)tag;
  (void)childPid;
  (void)memBytes;
  (void)pidsMax;
  note = "cgroup requires Linux";
  outPath.clear();
  return false;
#endif
}

namespace {

void initHelper(const ChildConfig& cfg) {
  std::vector<char*> cargv;
  cargv.reserve(cfg.cmd.size() + 1);
  for (const auto& a : cfg.cmd) {
    cargv.push_back(const_cast<char*>(a.c_str()));
  }
  cargv.push_back(nullptr);
  const pid_t agent = ::fork();
  if (agent < 0) {
    _exit(71);
  }
  if (agent == 0) {
    // prlimit fallback, agent-scoped (D1/CI: 22.04-asan dies if the
    // *observer* is address-limited — ASan needs vast address space, so
    // limits constrain only the about-to-exec agent, never middle/init).
    // NPROC+NOFILE always; AS only when --memory-max was explicit
    // (AS breaks Bun). Lowering as mapped root cannot fail loudly enough
    // to matter: failure here is a loud 70, never a silent unlimited run.
    if (cfg.pidsMax >= 0) {
      struct rlimit rl = {};
      rl.rlim_cur = static_cast<rlim_t>(cfg.pidsMax);
      rl.rlim_max = static_cast<rlim_t>(cfg.pidsMax);
      if (::setrlimit(RLIMIT_NPROC, &rl) != 0) {
        _exit(70);
      }
    }
    if (cfg.nofileMax >= 0) {
      struct rlimit rl = {};
      rl.rlim_cur = static_cast<rlim_t>(cfg.nofileMax);
      rl.rlim_max = static_cast<rlim_t>(cfg.nofileMax);
      if (::setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        _exit(70);
      }
    }
    if (cfg.memExplicit && cfg.memBytes >= 0) {
      struct rlimit rl = {};
      rl.rlim_cur = static_cast<rlim_t>(cfg.memBytes);
      rl.rlim_max = static_cast<rlim_t>(cfg.memBytes);
      if (::setrlimit(RLIMIT_AS, &rl) != 0) {
        _exit(70);
      }
    }
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

bool prepareRunDir(const std::string& runDir, const std::vector<std::string>& fsRwPaths,
                   OverlayDirs& dirs, std::vector<FsRwMount>& fsRw, std::string& error) {
#ifdef __linux__
  const std::string base = runDir + "/overlay";
  dirs.upper = base + "/upper";
  dirs.work = base + "/work";
  dirs.etcUpper = base + "/etc-upper";
  dirs.etcWork = base + "/etc-work";
  dirs.homeUpper = base + "/home-upper";
  dirs.homeWork = base + "/home-work";
  dirs.mnt = base + "/mnt";
  std::vector<std::string> fixed = {dirs.upper,     dirs.work,     dirs.etcUpper, dirs.etcWork,
                                    dirs.homeUpper, dirs.homeWork, dirs.mnt};
  for (size_t i = 0; i < fsRwPaths.size(); ++i) {
    FsRwMount m;
    m.path = fsRwPaths[i];
    m.upper = base + "/fs-rw-" + std::to_string(i);
    m.work = base + "/fs-rw-" + std::to_string(i) + "-work";
    fixed.push_back(m.upper);
    fixed.push_back(m.work);
    fsRw.push_back(m);
  }
  for (const std::string& d : fixed) {
    if (!mkpath(d, error)) {
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
  int a[2] = {-1, -1}, b[2] = {-1, -1}, c[2] = {-1, -1}, d[2] = {-1, -1};
  if (::pipe2(a, O_CLOEXEC) != 0 || ::pipe2(b, O_CLOEXEC) != 0 || ::pipe2(c, O_CLOEXEC) != 0 ||
      ::pipe2(d, O_CLOEXEC) != 0) {
    error = std::string("isolate: pipe: ") + errnoText(errno);
    return false;
  }
  p.mapReqR = a[0];
  p.mapReqW = a[1];
  p.mapAckR = b[0];
  p.mapAckW = b[1];
  p.statusR = c[0];
  p.statusW = c[1];
  p.envR = d[0];
  p.envW = d[1];
  return true;
#else
  (void)p;
  error = "isolate: requires Linux";
  return false;
#endif
}

void closeSupervisorEnds(ChildPipes& p) {
#ifdef __linux__
  for (int fd : {p.mapReqR, p.mapAckW, p.statusR, p.envW}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  p.mapReqR = p.mapAckW = p.statusR = p.envW = -1;
#else
  (void)p;
#endif
}

void closeMiddleEnds(ChildPipes& p) {
#ifdef __linux__
  for (int fd : {p.mapReqW, p.mapAckR, p.statusW, p.envR}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  p.mapReqW = p.mapAckR = p.statusW = p.envR = -1;
#else
  (void)p;
#endif
}

bool writeEnvBlock(int envW, const std::vector<std::pair<std::string, std::string>>& env,
                   std::string& error) {
#ifdef __linux__
  std::string block;
  for (const auto& kv : env) {
    if (kv.first.find('\n') != std::string::npos || kv.first.find('=') != std::string::npos ||
        kv.second.find('\n') != std::string::npos) {
      error = "isolate: refusing multiline env for " + kv.first;
      return false;
    }
    block += kv.first + "=" + kv.second + "\n";
  }
  block += "END\n";
  if (!writeAll(envW, block.data(), block.size())) {
    error = std::string("isolate: env block: ") + errnoText(errno);
    return false;
  }
  ::close(envW);
  return true;
#else
  (void)envW;
  (void)env;
  error = "isolate: requires Linux";
  return false;
#endif
}

#ifdef __linux__
// Bounded handshake reads: a stuck middle (e.g. a userspace-lock hang
// observed in-suite) must fail LOUD after 60s, never hang CI forever.
// Normal setup answers in milliseconds.
bool waitReadable(int fd, std::string& error) {
  for (;;) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int r = ::poll(&pfd, 1, 60000);
    if (r > 0) {
      return true;
    }
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r < 0) {
      error = std::string("isolate: handshake poll: ") + errnoText(errno);
    } else {
      error = "isolate: setup timed out after 60s (middle stuck?)";
    }
    return false;
  }
}
#endif

bool serveMaps(pid_t child, ChildPipes& p, std::string& error) {
#ifdef __linux__
  if (!waitReadable(p.mapReqR, error)) {
    return false;
  }
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
  if (!waitReadable(p.statusR, error)) {
    int st = 0;
    ::waitpid(child, &st, WNOHANG);
    return false;
  }
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
  // Debug trace: SG_ISOLATE_DEBUG=1 appends step markers to a file (used
  // to pin hangs; never on in normal runs).
  int dbgFd = -1;
  {
    const char* dp = ::getenv("SG_ISOLATE_DEBUG");
    if (dp != nullptr && dp[0] != '\0') {
      char dpn[64] = {};
      std::snprintf(dpn, sizeof(dpn), "/tmp/izdbg-%d.log", (int)::getpid());
      dbgFd = ::open(dpn, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
  }
#define IMARK(s)                                                                                   \
  do {                                                                                             \
    if (dbgFd >= 0) {                                                                              \
      (void)!::write(dbgFd, s "\n", sizeof(s "\n") - 1);                                           \
    }                                                                                              \
  } while (0)
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
  // The supervisor ignores SIGPIPE (EPIPE must surface as errors, never
  // an anonymous -13); the traced tree keeps default behavior instead.
  ::signal(SIGPIPE, SIG_DFL);
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    fail("unshare userns");
  }
  IMARK("unshared");
  // Map handshake FIRST, before any heap allocation: a multithreaded
  // supervisor under ASan may hold the allocator lock at fork, and the
  // middle would deadlock on its first malloc. Handshake uses no heap.
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
  const std::string repo = cfg.projectDir;
  const std::string merged = cfg.dirs.mnt;
  if (::getuid() != 0) {
    errno = EPERM;
    fail("idmap ineffective");
  }
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    fail("make-rprivate");
  }
  IMARK("rprivate");
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
  // Overlay targets: /etc (unless another target covers it), $HOME, the
  // repo, and each --fs-rw path. Top-level binds skip these and their
  // ancestors (mkdir-only); chains + mounts below are depth-sorted so
  // nesting (repo under $HOME, anything under /etc) mounts inside-out.
  struct OverlayJob {
    std::string lower;
    std::string target;
    std::string upper;
    std::string work;
    std::string what;
  };
  std::vector<OverlayJob> jobs = {
      {"/etc", "/etc", cfg.dirs.etcUpper, cfg.dirs.etcWork, "/etc"},
      {cfg.homeDir, cfg.homeDir, cfg.dirs.homeUpper, cfg.dirs.homeWork, "home"},
      {repo, repo, cfg.dirs.upper, cfg.dirs.work, "repo"},
  };
  for (const FsRwMount& m : cfg.fsRw) {
    jobs.push_back({m.path, m.path, m.upper, m.work, "fs-rw " + m.path});
  }
  std::vector<std::string> keep = {"/etc"};
  for (const auto& j : jobs) {
    keep.push_back(j.target);
    for (const std::string& a : ancestors(j.target)) {
      keep.push_back(a);
    }
  }
  const auto isKept = [&](const std::string& hostPath) {
    // Top-level readdir only: skip binds for overlay targets and their
    // ancestors (mkdir-only; mounts land below).
    for (const std::string& k : keep) {
      if (k == hostPath) {
        return true;
      }
      if (k.size() > hostPath.size() && k.compare(0, hostPath.size(), hostPath) == 0 &&
          k[hostPath.size()] == '/') {
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
        if (::symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
          fail("symlink " + dst);
        }
      }
      continue;
    }
    if (!S_ISDIR(st.st_mode)) {
      continue; // stray top-level files (none on FHS roots)
    }
    if (::mkdir(dst.c_str(), 0755) != 0 && errno != EEXIST) {
      fail("mkdir " + dst);
    }
    if (isKept(src)) {
      continue; // overlay target or ancestor: created empty, mounted below
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

  // Overlay chains (after the specials tmpfs mounts, so targets under
  // /tmp land on the fresh tmpfs) then mounts, shallowest first so
  // nesting (repo under $HOME, anything under /etc) stacks inside-out.
  // A job whose target another job also targets (only /etc can collide)
  // is skipped: the deeper mount wins.
  for (const auto& j : jobs) {
    std::string cur = merged;
    std::string rest = j.target.size() > 1 ? j.target.substr(1) : "";
    size_t i = 0;
    while (i < rest.size()) {
      const size_t k = rest.find('/', i);
      cur += "/" + rest.substr(i, k == std::string::npos ? k : k - i);
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        fail("mkdir chain " + cur);
      }
      if (k == std::string::npos) {
        break;
      }
      i = k + 1;
    }
  }
  auto depthOf = [](const std::string& p) { return std::count(p.begin(), p.end(), '/'); };
  std::stable_sort(jobs.begin(), jobs.end(), [&](const OverlayJob& a, const OverlayJob& b) {
    return depthOf(a.target) < depthOf(b.target);
  });
  size_t etcTargets = 0;
  for (const auto& j : jobs) {
    if (j.target == "/etc") {
      ++etcTargets;
    }
  }
  for (const auto& j : jobs) {
    if (j.target == "/etc" && etcTargets > 1 && j.what == "/etc") {
      continue; // a deeper job (repo/home/fs-rw at /etc) owns it
    }
    if (!mountOverlay(j.lower, j.upper, j.work, merged + j.target, walkErr, j.what.c_str())) {
      failMsg(walkErr);
    }
  }
  // Default secret-path masks (empty tmpfs, 0700): ~/.ssh, ~/.aws, ~/.gnupg.
  // Mounted over the merged $HOME overlay (lowerdir=host $HOME, so host keys
  // would otherwise be visible). --allow-path exempts (exact or parent).
  // ssh-based git remotes need --allow-path ~/.ssh (documented).
  {
    const std::vector<std::string> masks = defaultSecretMasks(cfg.homeDir, cfg.allowPath);
    for (const std::string& m : masks) {
      const std::string dst = merged + m;
      // mkdir -p the chain in the merged view (target may not exist on host).
      std::string cur;
      std::string rest = m.size() > 1 ? m.substr(1) : "";
      size_t i = 0;
      cur = merged;
      while (i < rest.size()) {
        const size_t k = rest.find('/', i);
        cur += "/" + rest.substr(i, k == std::string::npos ? k : k - i);
        if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
          fail("mkdir mask " + cur);
        }
        if (k == std::string::npos) {
          break;
        }
        i = k + 1;
      }
      if (::mount("tmpfs", dst.c_str(), "tmpfs", 0, "size=64m,mode=0700") != 0) {
        fail("mount tmpfs mask " + m);
      }
    }
  }

  if (::syscall(SYS_pivot_root, merged.c_str(), oldroot.c_str()) != 0) {
    fail("pivot_root");
  }
  IMARK("pivoted");
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
  IMARK("pidns");
  // PTRACE_TRACEME precedes the seccomp filter (ptrace(2) itself is
  // blocked afterwards — nested tracers get EPERM, by design).
  if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) {
    fail("traceme");
  }
  IMARK("traceme");
  {
    std::string secErr;
    if (!installIsolateFilter(secErr)) {
      failMsg(secErr);
    }
  }
  IMARK("seccomp");
  stripSecretEnv(cfg.allowEnv);
  IMARK("env");
  // Landlock LAST (one-way, inherited): RO world, RW islands. Depends on
  // NO_NEW_PRIVS, which the seccomp installer above has just set —
  // restrict_self fails EPERM without it on this kernel. Paths are
  // post-pivot merged-view absolutes (merged/X is /X after pivot).
  {
    std::vector<std::string> rw = {cfg.projectDir, cfg.homeDir, "/tmp", "/etc", "/dev", "/run"};
    for (const auto& j : jobs) {
      if (std::find(rw.begin(), rw.end(), j.target) == rw.end()) {
        rw.push_back(j.target);
      }
    }
    std::string llErr;
    if (!enforceLandlock(rw, llErr)) {
      failMsg(llErr);
    }
  }
  // prlimit fallback lives in the agent child just before exec (see
  // initHelper): limits constrain the agent, never this middle observer.
  // Injected env (proxy BASE_URLs): the middle forked before the proxy
  // existed, so the supervisor forwards them here. EOF (supervisor gone)
  // fails loud, never hangs: the status write below would EPIPE anyway.
  {
    std::string line;
    int got = 0;
    for (;;) {
      line.clear();
      char c = 0;
      bool nl = false;
      while (!nl) {
        ssize_t n = 0;
        do {
          n = ::read(cfg.envR, &c, 1);
        } while (n < 0 && errno == EINTR);
        if (n != 1) {
          errno = EPIPE;
          fail("env block EOF");
        }
        if (c == '\n') {
          nl = true;
        } else if (line.size() < 4096) {
          line.push_back(c);
        }
      }
      if (line == "END") {
        break;
      }
      if (got >= 64) {
        errno = E2BIG;
        fail("env block too large");
      }
      ++got;
      const size_t eq = line.find('=');
      if (eq == std::string::npos || eq == 0) {
        errno = EINVAL;
        fail("env block shape");
      }
      if (::setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1) != 0) {
        fail("env set");
      }
    }
  }
  IMARK("landlock");
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
    initHelper(cfg);
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

pid_t spawnMiddle(const ChildConfig& cfg, ChildPipes& pipes, std::string& error) {
#ifdef __linux__
  const pid_t child = ::fork();
  if (child < 0) {
    error = std::string("isolate: fork: ") + errnoText(errno);
    return -1;
  }
  if (child == 0) {
    closeSupervisorEnds(pipes);
    enterChild(cfg); // setups, stops, forks init, reaps, _exits
    _exit(72);       // unreachable
  }
  closeMiddleEnds(pipes);
  return child;
#else
  (void)cfg;
  (void)pipes;
  error = "isolate: requires Linux";
  return -1;
#endif
}

} // namespace snowglobe::isolate

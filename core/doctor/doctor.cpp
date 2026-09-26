#include "doctor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <sys/utsname.h>
#include <unistd.h>
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

}  // namespace

std::vector<Capability> checkAll() {
  std::vector<Capability> out;
#ifdef __linux__
  struct utsname u = {};
  uname(&u);
  out.push_back({"kernel", true, std::string(u.sysname) + " " + u.release + " " + u.machine});

  const std::string usernsCtl = readFile("/proc/sys/kernel/unprivileged_userns_clone");
  const bool hasUsernsFile = ::access("/proc/self/ns/user", F_OK) == 0;
  out.push_back({"user-namespace", hasUsernsFile,
                 usernsCtl.empty() ? (hasUsernsFile ? "present" : "no /proc/self/ns/user")
                                   : "unprivileged_userns_clone=" + usernsCtl});

  const std::string fs = readFile("/proc/filesystems");
  out.push_back({"overlayfs", fs.find("overlay") != std::string::npos,
                 fs.find("overlay") != std::string::npos ? "in /proc/filesystems"
                                                         : "overlay not listed (try probe/overlayfs_userns)"});

  // Landlock ABI: /proc/sys/abi/landlock exists on 5.13+.
  const bool landlock = ::access("/proc/sys/abi/landlock", F_OK) == 0;
  out.push_back({"landlock", landlock, landlock ? "ABI present" : "no /proc/sys/abi/landlock"});

  // seccomp user-notification needs 5.11+; best-effort: kernel version parse.
  int major = 0, minor = 0;
  std::sscanf(u.release, "%d.%d", &major, &minor);
  const bool seccompNotif = major > 5 || (major == 5 && minor >= 11);
  char ver[32] = {};
  std::snprintf(ver, sizeof(ver), "%d.%d", major, minor);
  out.push_back({"seccomp-notify", seccompNotif,
                 seccompNotif ? std::string("kernel ") + ver + " >= 5.11 (see probe/seccomp_notif)"
                              : std::string("kernel ") + ver + " < 5.11"});

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

}  // namespace snowglobe::doctor

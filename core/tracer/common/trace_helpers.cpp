// Shared tracee-memory/path helpers (see trace_helpers.hpp).
#include "trace_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../../redact/redact.hpp"
#include "../../util/string_util.hpp"

#ifndef __linux__
namespace snowglobe::tracer {
std::string errnoText(int e) {
  return "errno " + std::to_string(e);
}
uint64_t clockUs(int) {
  return 0;
}
long vmRead(int, uint64_t, char*, std::size_t) {
  return -1;
}
bool vmReadStr(int, uint64_t, std::string&, std::string& errDetail) {
  errDetail = "tracee reads require Linux";
  return false;
}
bool vmReadU64(int, uint64_t, uint64_t&) {
  return false;
}
std::string readLink(const std::string&) {
  return "";
}
int threadGroupId(int tid) {
  return tid;
}
std::string normaliseAbs(const std::string& p) {
  return p;
}
bool isNoisyPath(const std::string&) {
  return false;
}
std::string canonicalPath(int, long, const std::string& raw) {
  return raw;
}
bool readExecStrings(int, long, uint64_t, uint64_t,
                     const std::vector<std::pair<std::string, std::string>>&, std::string&,
                     std::string&, std::string& errDetail, bool&) {
  errDetail = "tracee reads require Linux";
  return false;
}
} // namespace snowglobe::tracer
#else

#include <fcntl.h>
#include <sys/uio.h>
#include <time.h>

namespace snowglobe::tracer {

std::string errnoText(int e) {
  char buf[128] = {};
  // GNU strerror_r returns char*.
  const char* m = strerror_r(e, buf, sizeof(buf));
  return m != nullptr ? std::string(m) : ("errno " + std::to_string(e));
}

uint64_t clockUs(int clk) {
  struct timespec ts = {};
  clock_gettime(clk, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

long vmRead(int pid, uint64_t remote, char* out, std::size_t maxLen) {
  struct iovec local = {out, maxLen};
  struct iovec remoteIov = {reinterpret_cast<void*>(static_cast<uintptr_t>(remote)), maxLen};
  const ssize_t n = process_vm_readv(pid, &local, 1, &remoteIov, 1, 0);
  return n < 0 ? -1 : static_cast<long>(n);
}

bool vmReadStr(int pid, uint64_t remote, std::string& out, std::string& errDetail) {
  out.clear();
  if (remote == 0) {
    errDetail = "null pointer";
    return false;
  }
  char buf[512];
  std::size_t total = 0;
  uint64_t cur = remote;
  while (total < 4352) {
    const long n = vmRead(pid, cur, buf, sizeof(buf));
    if (n <= 0) {
      errDetail = std::string("process_vm_readv: ") + errnoText(errno);
      return false;
    }
    for (long i = 0; i < n; ++i) {
      if (buf[i] == '\0') {
        out.append(buf, static_cast<std::size_t>(i));
        return true;
      }
    }
    out.append(buf, static_cast<std::size_t>(n));
    cur += static_cast<uint64_t>(n);
    total += static_cast<std::size_t>(n);
    if (n < static_cast<long>(sizeof(buf))) {
      break; // short read without NUL: string is truncated/unmapped
    }
  }
  errDetail = "string too long or unterminated";
  return false;
}

bool vmReadU64(int pid, uint64_t remote, uint64_t& out) {
  uint64_t v = 0;
  const long n = vmRead(pid, remote, reinterpret_cast<char*>(&v), sizeof(v));
  if (n != static_cast<long>(sizeof(v))) {
    return false;
  }
  out = v;
  return true;
}

std::string readLink(const std::string& path) {
  char buf[4096] = {};
  const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) {
    return "";
  }
  return std::string(buf, static_cast<std::size_t>(n));
}

int threadGroupId(int tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/status", tid);
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, 5, "Tgid:") == 0) {
      const int tgid = std::atoi(line.c_str() + 5);
      return tgid > 0 ? tgid : tid;
    }
  }
  return tid;
}

std::string normaliseAbs(const std::string& p) {
  std::vector<std::string> parts;
  std::string cur;
  const bool rooted = !p.empty() && p[0] == '/';
  for (std::size_t i = 0; i <= p.size(); ++i) {
    const char c = i < p.size() ? p[i] : '/';
    if (c == '/') {
      if (cur.empty() || cur == ".") {
        // skip
      } else if (cur == "..") {
        if (!parts.empty()) {
          parts.pop_back();
        }
      } else {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  std::string out = rooted ? "/" : "";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += "/";
    }
    out += parts[i];
  }
  if (out.empty()) {
    out = rooted ? "/" : ".";
  }
  return out;
}

bool isNoisyPath(const std::string& p) {
  static const char* kPrefixes[] = {"/proc/",
                                    "/sys/",
                                    "/dev/",
                                    "/etc/ld.so",
                                    "/etc/passwd",
                                    "/etc/nsswitch",
                                    "/usr/share/locale",
                                    "/usr/lib/locale"};
  for (const char* pre : kPrefixes) {
    if (p.compare(0, std::strlen(pre), pre) == 0) {
      return true;
    }
  }
  return p == "/proc" || p == "/sys" || p == "/dev";
}

std::string canonicalPath(int pid, long dirfd, const std::string& raw) {
  std::string base;
  if (!raw.empty() && raw[0] == '/') {
    base = raw;
  } else if (dirfd != AT_FDCWD) {
    const std::string fdLink = "/proc/" + std::to_string(pid) + "/fd/" + std::to_string(dirfd);
    base = readLink(fdLink);
    if (base.empty()) {
      base = readLink("/proc/" + std::to_string(pid) + "/cwd");
    }
    base += "/" + raw;
  } else {
    base = readLink("/proc/" + std::to_string(pid) + "/cwd");
    base += "/" + raw;
  }
  return normaliseAbs(base);
}

bool readExecStrings(int pid, long dirfd, uint64_t pathAddr, uint64_t argvAddr,
                     const std::vector<std::pair<std::string, std::string>>& secretEnv,
                     std::string& canonOut, std::string& argvJsonOut, std::string& detailOut,
                     bool& truncatedOut) {
  std::string path;
  if (!vmReadStr(pid, pathAddr, path, detailOut)) {
    return false;
  }
  std::vector<std::string> rawArgs;
  bool done = false;
  int got = 0;
  for (int i = 0; i < 64; ++i) {
    uint64_t p = 0;
    if (!vmReadU64(pid, argvAddr + static_cast<uint64_t>(i) * 8, p)) {
      break; // unreadable pointer slot: truncated
    }
    if (p == 0) {
      done = true; // argv terminator: complete
      break;
    }
    std::string s, d2;
    if (!vmReadStr(pid, p, s, d2)) {
      break; // unreadable string: truncated, keep the prefix
    }
    rawArgs.push_back(std::move(s));
    ++got;
  }
  truncatedOut = !done;
  if (truncatedOut && got == 64) {
    // Boundary check: exactly 64 args plus terminator is complete, not cut.
    uint64_t p = 1;
    if (vmReadU64(pid, argvAddr + 64 * 8, p) && p == 0) {
      truncatedOut = false;
    }
  }
  std::string argvJson;
  for (const std::string& raw : rawArgs) {
    if (!argvJson.empty()) {
      argvJson += ",";
    }
    argvJson += util::jsonEscape(redact::redactText(raw, secretEnv));
  }
  canonOut = canonicalPath(pid, dirfd, path);
  argvJsonOut = argvJson;
  return true;
}

} // namespace snowglobe::tracer
#endif

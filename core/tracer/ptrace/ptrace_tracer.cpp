// ptrace backend — Linux syscall tracer producing schema-v0 events.
// Thread ownership: single supervisor thread drives run(); no internal threads.
//
// Design notes:
//  - Child is started with PTRACE_TRACEME + SIGSTOP; parent sets
//    PTRACE_O_TRACEFORK|VFORK|CLONE|EXEC|EXITKILL and uses PTRACE_SYSCALL.
//  - Syscall enter/exit decoded via PTRACE_GET_SYSCALL_INFO (kernel >= 5.3).
//  - Strings/sockaddrs read via process_vm_readv. Any read failure emits
//    trace.decode_error — never a silent drop (AGENTS.md §3).
//  - Unknown pids (fork-race) are tracked as pending with ppid=-1 and
//    reconciled when the parent's fork event arrives.
//  - Threads: every event carries pid (=tgid) and tid; thread creation is a
//    PTRACE_EVENT_CLONE whose tgid (!= tid) is resolved via /proc/TID/status.
//    proc.start for threads sets thread:true.
//  - Exec in a multithreaded process: siblings may vanish with no exit stop
//    (or report a bare death first); the EXEC-event sweep emits proc.exit
//    with vanished:true for any still-tracked tids of the old tgid.
//  - Signals: first SIGINT/SIGTERM SIGTERMs the root only and keeps tracing
//    until the tree drains (manifest finalised); the second SIGKILLs all.
#include "ptrace_tracer.hpp"
#include "open_flags.hpp"

#include "../../redact/redact.hpp"
#include "../../util/string_util.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef __linux__
namespace snowglobe::tracer {
PtraceTracer* PtraceTracer::create() {
  return nullptr;
}
int PtraceTracer::run(const std::vector<std::string>&, const TraceOptions&) {
  error_ = "ptrace backend requires Linux";
  return -kExitUnavailable;
}
uint64_t PtraceTracer::nowTms() const {
  return 0;
}
uint64_t PtraceTracer::nowUs() const {
  return 0;
}
bool PtraceTracer::emitEv(const std::string&) {
  return true;
}
} // namespace snowglobe::tracer
#else

#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// PTRACE_GET_SYSCALL_INFO may be missing from old headers; define manually.
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
// Stop-type codes from UAPI <linux/ptrace.h> (enum ptrace_syscall_info_op).
// WARNING: the enum starts at NONE=0, so ENTRY=1, EXIT=2, SECCOMP=3.
// (An earlier revision used 0/1/2 and decoded zero syscalls; see probe/syscall_info.c.)
#ifndef PTRACE_SYSCALL_INFO_NONE
#define PTRACE_SYSCALL_INFO_NONE 0
#define PTRACE_SYSCALL_INFO_ENTRY 1
#define PTRACE_SYSCALL_INFO_EXIT 2
#define PTRACE_SYSCALL_INFO_SECCOMP 3
#endif
#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL (1 << 20)
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_EVENT_VFORK_DONE
#define PTRACE_EVENT_VFORK_DONE 5
#endif

// Minimal replica of <linux/ptrace.h> struct (avoid kernel-header dependency).
struct SgPtraceSyscallInfo {
  uint8_t op;
  uint8_t pad[3];
  uint32_t arch;
  uint64_t instruction_pointer;
  uint64_t stack_pointer;
  union {
    struct {
      uint64_t nr;
      uint64_t args[6];
    } entry;
    struct {
      int64_t rval;
      uint8_t is_error;
    } exit;
    struct {
      uint64_t nr;
      uint64_t args[6];
      uint32_t ret_data;
    } seccomp;
  };
};

namespace snowglobe::tracer {
namespace {

std::atomic<int> gStop{0};
void onSignal(int) {
  gStop.fetch_add(1);
}

uint64_t clockUs(clockid_t clk) {
  struct timespec ts = {};
  clock_gettime(clk, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

std::string errnoText(int e) {
  char buf[128] = {};
  // GNU strerror_r returns char*.
  const char* m = strerror_r(e, buf, sizeof(buf));
  return m != nullptr ? std::string(m) : ("errno " + std::to_string(e));
}

// Read up to maxLen bytes from remote address; returns bytes read or -1.
long vmRead(pid_t pid, uint64_t remote, char* out, std::size_t maxLen) {
  struct iovec local = {out, maxLen};
  struct iovec remoteIov = {reinterpret_cast<void*>(static_cast<uintptr_t>(remote)), maxLen};
  const ssize_t n = process_vm_readv(pid, &local, 1, &remoteIov, 1, 0);
  return n < 0 ? -1 : static_cast<long>(n);
}

// Read a NUL-terminated string from the tracee (up to 4096+256 bytes).
bool vmReadStr(pid_t pid, uint64_t remote, std::string& out, std::string& errDetail) {
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

bool vmReadU64(pid_t pid, uint64_t remote, uint64_t& out) {
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

// Resolve a tid's thread-group id via /proc. Leaders resolve to themselves;
// unreadable (already gone) resolves to tid — the tid attribution survives,
// the tgid may be approximate for flash-lived threads.
pid_t threadGroupId(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/status", (int)tid);
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, 5, "Tgid:") == 0) {
      const int tgid = std::atoi(line.c_str() + 5);
      return tgid > 0 ? (pid_t)tgid : tid;
    }
  }
  return tid;
}

// Normalise an absolute path lexically (no filesystem access).
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
  // Mirrors docs/trace-format.md §filtering: proc/sys/dev + loader/locale noise.
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

struct ProcInfo {
  pid_t ppid = -1;
  pid_t tgid = -1; // thread-group id (== tid for leaders; -1 = unresolved, use tid)
  bool isThread = false;
  bool inSyscall = false;
  uint64_t entryNr = 0;
  uint64_t entryArgs[6] = {};
  // execve path/argv captured at ENTRY (the old image is gone by EXIT time
  // after a successful exec, so exit-time reads fault with EFAULT).
  bool hasPendingExec = false;
  uint64_t pendingExecNr = 0;
  bool pendingExecTrunc = false;
  std::string pendingExecCanon;
  std::string pendingExecArgvJson;
};

} // namespace

PtraceTracer* PtraceTracer::create() {
  return new PtraceTracer();
}

uint64_t PtraceTracer::nowTms() const {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now =
      static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return now - tStartMs_;
}

uint64_t PtraceTracer::nowUs() const {
  return clockUs(CLOCK_REALTIME);
}

bool PtraceTracer::emitEv(const std::string& body) {
  return emit(body);
}

int PtraceTracer::run(const std::vector<std::string>& argv, const TraceOptions& opts) {
  using util::jsonEscape;
  if (argv.empty()) {
    error_ = "no command";
    return -kExitUsage;
  }
  allOpens_ = opts.allOpens;
  secretEnv_ = opts.secretEnv;

  struct timespec ts0 = {};
  clock_gettime(CLOCK_MONOTONIC, &ts0);
  tStartMs_ =
      static_cast<uint64_t>(ts0.tv_sec) * 1000ULL + static_cast<uint64_t>(ts0.tv_nsec) / 1000000ULL;

  // Install SIGINT/SIGTERM handlers (restore on exit).
  struct sigaction sa = {}, oldInt = {}, oldTerm = {};
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, &oldInt);
  sigaction(SIGTERM, &sa, &oldTerm);
  gStop.store(0);

  // Build argv for execvp.
  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& a : argv) {
    cargv.push_back(const_cast<char*>(a.c_str()));
  }
  cargv.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    error_ = std::string("fork: ") + errnoText(errno);
    sigaction(SIGINT, &oldInt, nullptr);
    sigaction(SIGTERM, &oldTerm, nullptr);
    return -kExitSoftware;
  }
  if (child == 0) {
    // --- tracee ---
    if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) {
      _exit(127);
    }
    ::raise(SIGSTOP);
    // libuv/Node file ops via io_uring bypass syscall tracing entirely;
    // force the syscall path unless the user overrode it. io_uring remains
    // a known blind spot (see docs/limitations.md in Block 3).
    ::setenv("UV_USE_IO_URING", "0", 0);
    ::execvp(cargv[0], cargv.data());
    _exit(127); // exec failed; parent reports proc.exec_failed via exit code 127 path
  }

  // --- supervisor ---
  std::map<pid_t, ProcInfo> procs;
  procs[child].ppid = ::getpid();
  procs[child].tgid = child; // root starts single-threaded
  int finalCode = 0;
  bool finalSet = false;

  // run.meta
  {
    std::string cmdJson;
    for (const auto& a : argv) {
      if (!cmdJson.empty()) {
        cmdJson += ",";
      }
      cmdJson += jsonEscape(a);
    }
    char cwd[4096] = {};
    std::string cwdStr;
    if (::getcwd(cwd, sizeof(cwd)) != nullptr) {
      cwdStr = cwd;
    }
    emitEv("{\"ts_us\":" + std::to_string(nowUs()) + ",\"t_ms\":" + std::to_string(nowTms()) +
           ",\"ev\":\"run.meta\",\"pid\":" + std::to_string(child) +
           ",\"tid\":" + std::to_string(child) + ",\"cmd\":[" + cmdJson +
           "],\"cwd\":" + jsonEscape(cwdStr) + "}");
    emitEv("{\"ts_us\":" + std::to_string(nowUs()) + ",\"t_ms\":" + std::to_string(nowTms()) +
           ",\"ev\":\"proc.start\",\"pid\":" + std::to_string(child) + ",\"tid\":" +
           std::to_string(child) + ",\"ppid\":" + std::to_string(::getpid()) + ",\"root\":true}");
  }

  const long kTraceOpts = PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
                          PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT | PTRACE_O_EXITKILL |
                          PTRACE_O_TRACESYSGOOD;
  bool firstStop = true;
  bool dead = false;
  int stopCount = 0;

  // Common envelope prefix with pid (=tgid) + tid resolution. Unknown tids
  // (fork-race window) fall back to pid == tid; attribution by tid survives.
  auto tsIds = [&](pid_t tid) -> std::string {
    pid_t tgid = tid;
    const auto it = procs.find(tid);
    if (it != procs.end() && it->second.tgid >= 0) {
      tgid = it->second.tgid;
    }
    return "\"ts_us\":" + std::to_string(nowUs()) + ",\"t_ms\":" + std::to_string(nowTms()) +
           ",\"pid\":" + std::to_string(tgid) + ",\"tid\":" + std::to_string(tid);
  };

  auto emitVanished = [&](pid_t tid) {
    emitEv("{" + tsIds(tid) + ",\"ev\":\"proc.exit\",\"vanished\":true}");
  };

  auto emitDecodeError = [&](pid_t tid, uint64_t nr, const std::string& reason) {
    const int e = errno; // capture first: tsIds() below must not clobber it
    emitEv("{" + tsIds(tid) + ",\"ev\":\"trace.decode_error\",\"syscall\":" + std::to_string(nr) +
           ",\"errno\":" + std::to_string(e) + ",\"reason\":" + jsonEscape(reason) + "}");
  };

  auto canonicalPath = [&](pid_t pid, long dirfd, const std::string& raw) -> std::string {
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
  };

  // Read an execve/execveat path+argv from the tracee *now* (valid only while
  // the calling image is still mapped — i.e. at ENTRY, or at EXIT on failure).
  // truncatedOut is set when argv was cut (64-entry cap or mid-array read
  // failure): callers report truncated:true rather than dropping silently.
  // Every argv element is secret-redacted (ADR-0003) before escaping.
  auto readExecStrings = [&](pid_t tpid, long dirfd, uint64_t pathAddr, uint64_t argvAddr,
                             std::string& canonOut, std::string& argvJsonOut,
                             std::string& detailOut, bool& truncatedOut) -> bool {
    std::string path;
    if (!vmReadStr(tpid, pathAddr, path, detailOut)) {
      return false;
    }
    std::vector<std::string> rawArgs;
    bool done = false;
    int got = 0;
    for (int i = 0; i < 64; ++i) {
      uint64_t p = 0;
      if (!vmReadU64(tpid, argvAddr + static_cast<uint64_t>(i) * 8, p)) {
        break; // unreadable pointer slot: truncated
      }
      if (p == 0) {
        done = true; // argv terminator: complete
        break;
      }
      std::string s, d2;
      if (!vmReadStr(tpid, p, s, d2)) {
        break; // unreadable string: truncated, keep the prefix
      }
      rawArgs.push_back(std::move(s));
      ++got;
    }
    truncatedOut = !done;
    if (truncatedOut && got == 64) {
      // Boundary check: exactly 64 args plus terminator is complete, not cut.
      uint64_t p = 1;
      if (vmReadU64(tpid, argvAddr + 64 * 8, p) && p == 0) {
        truncatedOut = false;
      }
    }
    std::string argvJson;
    for (const std::string& raw : rawArgs) {
      if (!argvJson.empty()) {
        argvJson += ",";
      }
      argvJson += jsonEscape(redact::redactText(raw, secretEnv_));
    }
    canonOut = canonicalPath(tpid, dirfd, path);
    argvJsonOut = argvJson;
    return true;
  };

  while (!dead) {
    // Consume pending signals as a count: two rapid SIGINTs must not
    // coalesce into one (second Ctrl-C means SIGKILL).
    const int pending = gStop.exchange(0);
    if (pending > 0) {
      stopCount += pending;
      if (stopCount == 1) {
        // First interrupt: SIGTERM the root only, then keep tracing until
        // the tree drains. Exits (incl. signal deaths) are recorded normally
        // and the manifest is finalised with the root's exit code.
        if (procs.find(child) != procs.end()) {
          ::kill(child, SIGTERM);
        } else {
          for (const auto& [tid, _] : procs) {
            ::kill(tid, SIGTERM);
          }
        }
      } else {
        // Second interrupt: SIGKILL everything, leave a partial trace.
        for (const auto& [tid, _] : procs) {
          ::kill(tid, SIGKILL);
        }
        error_ = "interrupted";
        break;
      }
    }
    int status = 0;
    const pid_t pid = ::waitpid(-1, &status, __WALL);
    if (pid < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        // No waitable tracees left but entries remain (e.g. threads that
        // vanished in an exec race we never saw): close them out honestly
        // rather than hanging or dropping them silently.
        for (const auto& [tid, _] : procs) {
          emitVanished(tid);
        }
        procs.clear();
        break;
      }
      error_ = std::string("waitpid: ") + errnoText(errno);
      finalCode = kExitSoftware;
      finalSet = true;
      break;
    }
    if (procs.find(pid) == procs.end()) {
      // Fork-race: child stopped before the parent's fork event arrived.
      procs[pid].ppid = -1;
    }

    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      const int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
      if (firstStop && pid == child) {
        // The tracee always stops at SIGSTOP before exec: dying first means
        // either PTRACE_TRACEME failed (already under another tracer — a
        // supervisor failure, never the agent's exit code) or an external
        // SIGKILL/SIGTERM in the microsecond window (a genuine death).
        firstStop = false;
        if (WIFEXITED(status)) {
          procs.erase(pid);
          error_ = "tracee exited before first stop (already traced elsewhere?)";
          finalCode = kExitSoftware;
          finalSet = true;
          break;
        }
      }
      emitEv("{" + tsIds(pid) + ",\"ev\":\"proc.exit\",\"code\":" + std::to_string(code) +
             ",\"signal\":" + std::to_string(sig) + "}");
      // exec-failed detection: first process exiting 127 without a prior exec event
      // is reported as proc.exec_failed for visibility.
      procs.erase(pid);
      if (pid == child && !finalSet) {
        finalCode = code;
        finalSet = true;
      }
      if (procs.empty()) {
        dead = true;
      }
      continue;
    }
    if (!WIFSTOPPED(status)) {
      continue;
    }
    const int sig = WSTOPSIG(status);
    const unsigned event = (static_cast<unsigned>(status) >> 16) & 0xffff;

    if (firstStop && pid == child && sig == SIGSTOP) {
      firstStop = false;
      ptrace(PTRACE_SETOPTIONS, pid, nullptr, reinterpret_cast<void*>(kTraceOpts));
      ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
      continue;
    }

    if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
      unsigned long msg = 0;
      if (ptrace(PTRACE_GETEVENTMSG, pid, nullptr, &msg) != 0) {
        // ESRCH: tracee died mid-event; its exit will be reaped next.
        ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
        continue;
      }
      const auto cpid = static_cast<pid_t>(msg);
      auto& ci = procs[cpid]; // reconciles fork-race pending entries
      ci.ppid = pid;
      ci.inSyscall = false;
      // Thread or process? A CLONE event covers both (and clone3); resolve
      // via /proc. Threads get pid=tgid + thread:true.
      const pid_t tgid = threadGroupId(cpid);
      ci.tgid = tgid;
      ci.isThread = (tgid != cpid);
      emitEv("{" + tsIds(cpid) + ",\"ev\":\"proc.start\",\"ppid\":" + std::to_string(pid) +
             (ci.isThread ? ",\"thread\":true}" : "}"));
      ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
      continue;
    }
    if (event == PTRACE_EVENT_EXEC) {
      // Siblings of the old image may vanish with no exit stop (or report a
      // bare death first, which the normal exit path already consumed):
      // close out anything still tracked under the old tgid. The execve
      // syscall-exit decoder still emits the single proc.exec.
      ProcInfo& ex = procs[pid];
      const pid_t oldTgid = ex.tgid < 0 ? pid : ex.tgid;
      std::vector<pid_t> gone;
      for (const auto& [tid, info] : procs) {
        if (tid != pid && info.tgid == oldTgid) {
          gone.push_back(tid);
        }
      }
      for (const pid_t t : gone) {
        // Non-leader exec: the ENTRY (with path/argv) was recorded on the
        // vanishing sibling while the EXIT lands on the surviving tid.
        // Scavenge it so the single proc.exec is still emitted.
        if (!ex.hasPendingExec) {
          const ProcInfo& g = procs[t];
          if (g.hasPendingExec) {
            ex.hasPendingExec = true;
            ex.pendingExecNr = g.pendingExecNr;
            ex.pendingExecTrunc = g.pendingExecTrunc;
            ex.pendingExecCanon = g.pendingExecCanon;
            ex.pendingExecArgvJson = g.pendingExecArgvJson;
            ex.inSyscall = true;
            ex.entryNr = g.pendingExecNr;
          }
        }
        emitVanished(t);
        procs.erase(t);
      }
      ex.tgid = pid;
      ex.isThread = false;
      ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
      continue;
    }
    if (event == PTRACE_EVENT_EXIT) {
      ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
      continue;
    }
    if (event == PTRACE_EVENT_VFORK_DONE) {
      // vfork parent unblocked (child exec'd or exited): just resume.
      ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
      continue;
    }

    if (sig == (SIGTRAP | 0x80)) {
      // Syscall stop: decode enter/exit.
      SgPtraceSyscallInfo info = {};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, reinterpret_cast<void*>(sizeof(info)),
                 reinterpret_cast<void*>(&info)) < 0) {
        // ESRCH: tracee died between the stop and the read; its exit is next.
        // Anything else is a real decoder failure — report it, never swallow.
        if (errno != ESRCH) {
          emitDecodeError(pid, 0, std::string("PTRACE_GET_SYSCALL_INFO: ") + errnoText(errno));
        }
        ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
        continue;
      }
      ProcInfo& pi = procs[pid];
      if (info.op == PTRACE_SYSCALL_INFO_ENTRY) {
        pi.inSyscall = true;
        pi.entryNr = info.entry.nr;
        for (int i = 0; i < 6; ++i) {
          pi.entryArgs[i] = info.entry.args[i];
        }
        pi.hasPendingExec = false;
        // Cache execve path+argv NOW: after a successful exec the old image
        // (and these pointers) are gone by EXIT time.
        const uint64_t enr = info.entry.nr;
        bool isExec = false;
#ifdef SYS_execve
        isExec = isExec || (enr == static_cast<uint64_t>(SYS_execve));
#endif
#ifdef SYS_execveat
        isExec = isExec || (enr == static_cast<uint64_t>(SYS_execveat));
#endif
        if (isExec) {
          const bool isAt =
#ifdef SYS_execveat
              (enr == static_cast<uint64_t>(SYS_execveat));
#else
              false;
#endif
          const uint64_t pathAddr = isAt ? info.entry.args[1] : info.entry.args[0];
          const uint64_t argvAddr = isAt ? info.entry.args[2] : info.entry.args[1];
          const long dirfd = isAt ? static_cast<long>(info.entry.args[0]) : AT_FDCWD;
          std::string detail;
          pi.pendingExecTrunc = false;
          if (readExecStrings(pid, dirfd, pathAddr, argvAddr, pi.pendingExecCanon,
                              pi.pendingExecArgvJson, detail, pi.pendingExecTrunc)) {
            pi.hasPendingExec = true;
            pi.pendingExecNr = enr;
          }
        }
      } else if (info.op == PTRACE_SYSCALL_INFO_EXIT && pi.inSyscall) {
        pi.inSyscall = false;
        const uint64_t nr = pi.entryNr;
        const int64_t rval = info.exit.rval;
        const bool isErr = info.exit.is_error != 0;

        // Classify with portability guards (numbers differ per arch).
        enum class Kind {
          None,
          Exec,
          Open,
          Unlink,
          Rename,
          Mkdir,
          Connect,
          Sendto,
          Bind,
          Symlink,
          Chmod
        };
        Kind kind = Kind::None;
#ifdef SYS_execve
        if (nr == static_cast<uint64_t>(SYS_execve)) {
          kind = Kind::Exec;
        }
#endif
#ifdef SYS_execveat
        if (nr == static_cast<uint64_t>(SYS_execveat)) {
          kind = Kind::Exec;
        }
#endif
#ifdef SYS_openat
        if (nr == static_cast<uint64_t>(SYS_openat)) {
          kind = Kind::Open;
        }
#endif
#ifdef SYS_open
        if (nr == static_cast<uint64_t>(SYS_open)) {
          kind = Kind::Open;
        }
#endif
#ifdef SYS_openat2
        if (nr == static_cast<uint64_t>(SYS_openat2)) {
          kind = Kind::Open;
        }
#endif
#ifdef SYS_creat
        if (nr == static_cast<uint64_t>(SYS_creat)) {
          kind = Kind::Open;
        }
#endif
#ifdef SYS_unlink
        if (nr == static_cast<uint64_t>(SYS_unlink)) {
          kind = Kind::Unlink;
        }
#endif
#ifdef SYS_unlinkat
        if (nr == static_cast<uint64_t>(SYS_unlinkat)) {
          kind = Kind::Unlink;
        }
#endif
#ifdef SYS_rmdir
        // rmdir(2): glibc emits the legacy syscall, not unlinkat.
        if (nr == static_cast<uint64_t>(SYS_rmdir)) {
          kind = Kind::Unlink;
        }
#endif
#ifdef SYS_rename
        if (nr == static_cast<uint64_t>(SYS_rename)) {
          kind = Kind::Rename;
        }
#endif
#ifdef SYS_renameat
        if (nr == static_cast<uint64_t>(SYS_renameat)) {
          kind = Kind::Rename;
        }
#endif
#ifdef SYS_renameat2
        if (nr == static_cast<uint64_t>(SYS_renameat2)) {
          kind = Kind::Rename;
        }
#endif
#ifdef SYS_mkdir
        if (nr == static_cast<uint64_t>(SYS_mkdir)) {
          kind = Kind::Mkdir;
        }
#endif
#ifdef SYS_mkdirat
        if (nr == static_cast<uint64_t>(SYS_mkdirat)) {
          kind = Kind::Mkdir;
        }
#endif
#ifdef SYS_connect
        if (nr == static_cast<uint64_t>(SYS_connect)) {
          kind = Kind::Connect;
        }
#endif
#ifdef SYS_sendto
        if (nr == static_cast<uint64_t>(SYS_sendto)) {
          kind = Kind::Sendto;
        }
#endif
#ifdef SYS_bind
        if (nr == static_cast<uint64_t>(SYS_bind)) {
          kind = Kind::Bind;
        }
#endif
#ifdef SYS_symlink
        if (nr == static_cast<uint64_t>(SYS_symlink)) {
          kind = Kind::Symlink;
        }
#endif
#ifdef SYS_symlinkat
        if (nr == static_cast<uint64_t>(SYS_symlinkat)) {
          kind = Kind::Symlink;
        }
#endif
#ifdef SYS_chmod
        if (nr == static_cast<uint64_t>(SYS_chmod)) {
          kind = Kind::Chmod;
        }
#endif
#ifdef SYS_fchmod
        if (nr == static_cast<uint64_t>(SYS_fchmod)) {
          kind = Kind::Chmod;
        }
#endif
#ifdef SYS_fchmodat
        if (nr == static_cast<uint64_t>(SYS_fchmodat)) {
          kind = Kind::Chmod;
        }
#endif

        if (kind != Kind::None) {
          const std::string ts = tsIds(pid);
          switch (kind) {
          case Kind::Exec: {
            // execve(path, argv, envp) / execveat(dirfd, path, argv, envp, flags).
            // Success: use the ENTRY-time cache (old image is gone now).
            // Failure: re-read live (old image still mapped — most accurate).
            const std::string cwd = readLink("/proc/" + std::to_string(pid) + "/cwd");
            if (!isErr && pi.hasPendingExec) {
              emitEv("{" + ts +
                     ",\"ev\":\"proc.exec\",\"path\":" + jsonEscape(pi.pendingExecCanon) +
                     ",\"argv\":[" + pi.pendingExecArgvJson + "],\"cwd\":" + jsonEscape(cwd) +
                     (pi.pendingExecTrunc ? ",\"truncated\":true}" : "}"));
              pi.hasPendingExec = false;
              pi.pendingExecTrunc = false;
              break;
            }
            const bool isAt =
#ifdef SYS_execveat
                (nr == static_cast<uint64_t>(SYS_execveat));
#else
                false;
#endif
            const uint64_t pathAddr = isAt ? pi.entryArgs[1] : pi.entryArgs[0];
            const uint64_t argvAddr = isAt ? pi.entryArgs[2] : pi.entryArgs[1];
            const long dirfd = isAt ? static_cast<long>(pi.entryArgs[0]) : AT_FDCWD;
            if (!isErr) {
              // No entry cache (e.g. tracer attached mid-syscall): best effort.
              std::string detail, canon, argvJson;
              bool truncated = false;
              if (!readExecStrings(pid, dirfd, pathAddr, argvAddr, canon, argvJson, detail,
                                   truncated)) {
                emitDecodeError(pid, nr, "exec path: " + detail);
                break;
              }
              emitEv("{" + ts + ",\"ev\":\"proc.exec\",\"path\":" + jsonEscape(canon) +
                     ",\"argv\":[" + argvJson + "],\"cwd\":" + jsonEscape(cwd) +
                     (truncated ? ",\"truncated\":true}" : "}"));
              break;
            }
            std::string path, detail;
            if (!vmReadStr(pid, pathAddr, path, detail)) {
              emitDecodeError(pid, nr, "exec path: " + detail);
              break;
            }
            emitEv("{" + ts + ",\"ev\":\"proc.exec_failed\",\"path\":" + jsonEscape(path) +
                   ",\"errno\":" + std::to_string(-rval) + "}");
            pi.hasPendingExec = false;
            break;
          }
          case Kind::Open: {
            // open(path, flags) / openat(dirfd, path, flags) / creat(path, mode)
            // openat2(dirfd, path, open_how*, size): flags live in the struct.
            bool isCreat = false;
#ifdef SYS_creat
            isCreat = (nr == static_cast<uint64_t>(SYS_creat));
#endif
            bool isOpenat2 = false;
#ifdef SYS_openat2
            isOpenat2 = (nr == static_cast<uint64_t>(SYS_openat2));
#endif
            long dirfd = AT_FDCWD;
            uint64_t pathAddr = pi.entryArgs[0];
            uint64_t flagArg = 0;
#ifdef SYS_openat
            if (nr == static_cast<uint64_t>(SYS_openat)) {
              dirfd = static_cast<long>(pi.entryArgs[0]);
              pathAddr = pi.entryArgs[1];
              flagArg = pi.entryArgs[2];
            }
#endif
#ifdef SYS_open
            if (nr == static_cast<uint64_t>(SYS_open)) {
              flagArg = pi.entryArgs[1];
            }
#endif
            if (isOpenat2) {
              dirfd = static_cast<long>(pi.entryArgs[0]);
              pathAddr = pi.entryArgs[1];
              // struct open_how starts with flags (__u64 at offset 0).
              if (!vmReadU64(pid, pi.entryArgs[2], flagArg)) {
                emitDecodeError(pid, nr, "openat2 open_how: unreadable");
                break;
              }
            }
            const OpenFlags of = classifyOpenFlags(flagArg, isCreat);
            if (!allOpens_ && of.dirOrPath) {
              break; // default filter: O_DIRECTORY / O_PATH opens carry no content
            }
            std::string path, detail;
            if (!vmReadStr(pid, pathAddr, path, detail)) {
              emitDecodeError(pid, nr, "open path: " + detail);
              break;
            }
            const std::string canon = canonicalPath(pid, dirfd, path);
            const bool okCall = !isErr;
            if (!allOpens_ && !of.write) {
              if (!okCall || isNoisyPath(canon)) {
                break; // default filter: failed read-opens + noisy paths
              }
            }
            const long fd = okCall ? static_cast<long>(rval) : -1;
            emitEv("{" + ts + ",\"ev\":\"fs.open\",\"path\":" + jsonEscape(canon) + ",\"write\":" +
                   (of.write ? "true" : "false") + ",\"create\":" + (of.create ? "true" : "false") +
                   ",\"trunc\":" + (of.trunc ? "true" : "false") + ",\"fd\":" + std::to_string(fd) +
                   (of.tmpfile ? ",\"tmpfile\":true" : "") + "}");
            break;
          }
          case Kind::Unlink: {
            bool isAt = false;
#ifdef SYS_unlinkat
            isAt = (nr == static_cast<uint64_t>(SYS_unlinkat));
#endif
            long dirfd = AT_FDCWD;
            uint64_t pathAddr = pi.entryArgs[0];
            if (isAt) {
              dirfd = static_cast<long>(pi.entryArgs[0]);
              pathAddr = pi.entryArgs[1];
            }
            // unlinkat with AT_REMOVEDIR (or the legacy rmdir syscall)
            // removes a directory: report fs.rmdir.
            bool rmdir = false;
#ifdef SYS_rmdir
            rmdir = (nr == static_cast<uint64_t>(SYS_rmdir));
#endif
#ifdef AT_REMOVEDIR
            rmdir = rmdir || (isAt && ((pi.entryArgs[2] & AT_REMOVEDIR) != 0));
#endif
            std::string path, detail;
            if (!vmReadStr(pid, pathAddr, path, detail)) {
              emitDecodeError(pid, nr, "unlink path: " + detail);
              break;
            }
            emitEv("{" + ts + ",\"ev\":\"" + (rmdir ? "fs.rmdir" : "fs.unlink") +
                   "\",\"path\":" + jsonEscape(canonicalPath(pid, dirfd, path)) +
                   ",\"ok\":" + (!isErr ? "true" : "false") + "}");
            break;
          }
          case Kind::Rename: {
            bool twoDir = false;
#ifdef SYS_renameat
            twoDir = (nr == static_cast<uint64_t>(SYS_renameat));
#endif
#ifdef SYS_renameat2
            twoDir = twoDir || (nr == static_cast<uint64_t>(SYS_renameat2));
#endif
            long fromDir = AT_FDCWD, toDir = AT_FDCWD;
            uint64_t fromAddr = pi.entryArgs[0], toAddr = pi.entryArgs[1];
            if (twoDir) {
              fromDir = static_cast<long>(pi.entryArgs[0]);
              fromAddr = pi.entryArgs[1];
              toDir = static_cast<long>(pi.entryArgs[2]);
              toAddr = pi.entryArgs[3];
            }
            std::string from, to, d;
            if (!vmReadStr(pid, fromAddr, from, d)) {
              emitDecodeError(pid, nr, "rename from: " + d);
              break;
            }
            if (!vmReadStr(pid, toAddr, to, d)) {
              emitDecodeError(pid, nr, "rename to: " + d);
              break;
            }
            emitEv("{" + ts + ",\"ev\":\"fs.rename\",\"from\":" +
                   jsonEscape(canonicalPath(pid, fromDir, from)) +
                   ",\"to\":" + jsonEscape(canonicalPath(pid, toDir, to)) +
                   ",\"ok\":" + (!isErr ? "true" : "false") + "}");
            break;
          }
          case Kind::Mkdir: {
            bool isAt = false;
#ifdef SYS_mkdirat
            isAt = (nr == static_cast<uint64_t>(SYS_mkdirat));
#endif
            long dirfd = AT_FDCWD;
            uint64_t pathAddr = pi.entryArgs[0];
            if (isAt) {
              dirfd = static_cast<long>(pi.entryArgs[0]);
              pathAddr = pi.entryArgs[1];
            }
            std::string path, detail;
            if (!vmReadStr(pid, pathAddr, path, detail)) {
              emitDecodeError(pid, nr, "mkdir path: " + detail);
              break;
            }
            emitEv("{" + ts + ",\"ev\":\"fs.mkdir\",\"path\":" +
                   jsonEscape(canonicalPath(pid, dirfd, path)) + "}");
            break;
          }
          case Kind::Symlink: {
            // symlink(target, link) / symlinkat(target, dirfd, link)
            bool isAt = false;
#ifdef SYS_symlinkat
            isAt = (nr == static_cast<uint64_t>(SYS_symlinkat));
#endif
            uint64_t targetAddr = pi.entryArgs[0];
            long dirfd = AT_FDCWD;
            uint64_t linkAddr = pi.entryArgs[1];
            if (isAt) {
              dirfd = static_cast<long>(pi.entryArgs[1]);
              linkAddr = pi.entryArgs[2];
            }
            std::string target, link, d;
            if (!vmReadStr(pid, targetAddr, target, d)) {
              emitDecodeError(pid, nr, "symlink target: " + d);
              break;
            }
            if (!vmReadStr(pid, linkAddr, link, d)) {
              emitDecodeError(pid, nr, "symlink path: " + d);
              break;
            }
            emitEv("{" + ts + ",\"ev\":\"fs.symlink\",\"target\":" + jsonEscape(target) +
                   ",\"path\":" + jsonEscape(canonicalPath(pid, dirfd, link)) +
                   ",\"ok\":" + (!isErr ? "true" : "false") + "}");
            break;
          }
          case Kind::Chmod: {
            // chmod(path, mode) / fchmodat(dirfd, path, mode) / fchmod(fd, mode).
            // mode is an octal string ("0755"), not a number.
            bool isFd = false;
#ifdef SYS_fchmod
            isFd = (nr == static_cast<uint64_t>(SYS_fchmod));
#endif
            bool isAt = false;
#ifdef SYS_fchmodat
            isAt = (nr == static_cast<uint64_t>(SYS_fchmodat));
#endif
            std::string path;
            uint64_t mode = 0;
            if (isFd) {
              const long fd = static_cast<long>(pi.entryArgs[0]);
              mode = pi.entryArgs[1];
              path = readLink("/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd));
              if (path.empty()) {
                emitDecodeError(pid, nr, "fchmod fd has no path");
                break;
              }
              path = normaliseAbs(path);
            } else {
              long dirfd = AT_FDCWD;
              uint64_t pathAddr = pi.entryArgs[0];
              mode = pi.entryArgs[1];
              if (isAt) {
                dirfd = static_cast<long>(pi.entryArgs[0]);
                pathAddr = pi.entryArgs[1];
                mode = pi.entryArgs[2];
              }
              std::string raw, detail;
              if (!vmReadStr(pid, pathAddr, raw, detail)) {
                emitDecodeError(pid, nr, "chmod path: " + detail);
                break;
              }
              path = canonicalPath(pid, dirfd, raw);
            }
            // Mode is an octal string ("0755"): stable across readers, no
            // decimal/octal ambiguity in goldens.
            char modeStr[16] = {};
            std::snprintf(modeStr, sizeof(modeStr), "0%o", (unsigned int)mode);
            emitEv("{" + ts + ",\"ev\":\"fs.chmod\",\"path\":" + jsonEscape(path) + ",\"mode\":" +
                   jsonEscape(modeStr) + ",\"ok\":" + (!isErr ? "true" : "false") + "}");
            break;
          }
          case Kind::Connect:
          case Kind::Sendto:
          case Kind::Bind: {
            // connect(fd, addr, len) / sendto(fd, buf, len, flags, addr, len)
            uint64_t addrArg = pi.entryArgs[1];
            uint64_t lenArg = pi.entryArgs[2];
            if (kind == Kind::Sendto) {
              addrArg = pi.entryArgs[4];
              lenArg = pi.entryArgs[5];
            }
            const char* evName = kind == Kind::Connect
                                     ? "net.connect"
                                     : (kind == Kind::Sendto ? "net.sendto" : "net.bind");
            if (addrArg == 0 || lenArg == 0) {
              if (kind == Kind::Connect) {
                bool initiated = !isErr;
#ifdef EINPROGRESS
                if (isErr && rval == -EINPROGRESS) {
                  initiated = true;
                }
#endif
                emitEv("{" + ts + ",\"ev\":\"net.connect\",\"family\":\"unknown\"" +
                       (initiated ? ",\"initiated\":true}" : ",\"initiated\":false}"));
              }
              break;
            }
            if (lenArg > 256) {
              // No real sockaddr is this large; report instead of dropping.
              emitDecodeError(pid, nr, "sockaddr too long");
              break;
            }
            char sbuf[256] = {};
            const std::size_t wantLen = static_cast<std::size_t>(lenArg) > sizeof(sbuf)
                                            ? sizeof(sbuf)
                                            : static_cast<std::size_t>(lenArg);
            if (vmRead(pid, addrArg, sbuf, wantLen) < 0) {
              emitDecodeError(pid, nr, std::string("sockaddr: ") + errnoText(errno));
              break;
            }
            const std::string formatted =
                util::formatSockaddr(sbuf, static_cast<unsigned long>(wantLen));
            // initiated: the kernel took the connection attempt (success or
            // EINPROGRESS for non-blocking). Immediate failures (refused,
            // unreachable) never initiated.
            bool initiated = !isErr;
#ifdef EINPROGRESS
            if (isErr && rval == -EINPROGRESS) {
              initiated = true;
            }
#endif
            emitEv("{" + ts + ",\"ev\":\"" + evName + "\",\"addr\":" + jsonEscape(formatted) +
                   ",\"ok\":" + (!isErr ? "true" : "false") +
                   (kind == Kind::Connect
                        ? (initiated ? ",\"initiated\":true" : ",\"initiated\":false")
                        : "") +
                   "}");
            break;
          }
          case Kind::None: break;
          }
        }
      }
      ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
      continue;
    }

    // Resume strategy: event stops (EXEC/EXIT/VFORK_DONE/...) and SIGSTOP
    // noise resume bare. A bare SIGTRAP with no event is a stray trap, also
    // resumed bare. Anything else is a real signal for the tracee. In
    // particular we never inject SIGTRAP into the tracee on event stops.
    // NOTE: SIGCHLD is always a genuine delivery (the supervisor may have
    // stolen the reaping, but the tracee's job control still needs the
    // notification): it must be forwarded, never swallowed.
    if (sig == SIGSTOP || event != 0 || sig == SIGTRAP) {
      ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
    } else {
      ptrace(PTRACE_SYSCALL, pid, nullptr, reinterpret_cast<void*>(static_cast<long>(sig)));
    }
  }

  // No orphans: ensure everything is reaped/killed.
  for (const auto& [pid, _] : procs) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, WNOHANG);
  }

  sigaction(SIGINT, &oldInt, nullptr);
  sigaction(SIGTERM, &oldTerm, nullptr);
  if (!error_.empty()) {
    // Supervisor failure (interrupted, waitpid error, tracee gone before
    // first stop): loud EX_SOFTWARE with a partial manifest, never a fake
    // agent exit code. The CLI prints error_ in this path.
    return -kExitSoftware;
  }
  if (!finalSet) {
    // The tree is gone but the root never reported (exec-vanish race the
    // sweep missed, or an ECHILD drain): partial trace, honest failure.
    error_ = "lost track of root exit status";
    return -kExitSoftware;
  }
  return finalCode;
}

} // namespace snowglobe::tracer

#endif // __linux__

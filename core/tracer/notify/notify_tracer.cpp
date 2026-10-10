// seccomp user-notification backend (ADR-0009, Phase 4 Block 2).
// Thread ownership: single supervisor thread drives run(); no internal threads.
//
// How it works: the child installs a seccomp filter (default ALLOW,
// USER_NOTIF on exactly the syscalls we decode) just before exec and
// hands the listener fd to the supervisor over a socketpair. Process
// lifecycle (fork/clone/exec/exit) arrives via PTRACE_SEIZE + TRACE*
// ptrace events; observed syscalls arrive as notifications on the
// listener fd. The supervisor polls the listener alongside waitpid and
// answers every notification with CONTINUE — promptly, never holding a
// blocked syscall hostage.
//
// Entry-only observation is the honest semantic delta (schema stays 0):
// there are no syscall-exit return values, so outcome events carry
// result_known:false instead of ok/errno/fd. run.meta carries
// backend:notify. Everything entry-known (paths, argv, flags, sockaddr
// endpoints) is decoded by the SAME helpers as the ptrace backend
// (trace_helpers.hpp) — parity by construction.
//
// Death safety: every valid notification gets exactly one SEND answer;
// decode failures emit trace.decode_error and still answer. Supervisor
// death kills tracees via PTRACE_O_EXITKILL even mid-notification —
// proven by the notify kill tests, not assumed.
#include "notify_tracer.hpp"

#include <string>
#include <utility>
#include <vector>

#ifndef __linux__
namespace snowglobe::tracer {
NotifyTracer* NotifyTracer::create() {
  return nullptr;
}
bool NotifyTracer::probeAvailable(std::string& reason) {
  reason = "seccomp user-notify requires Linux";
  return false;
}
int NotifyTracer::run(const std::vector<std::string>&, const TraceOptions&) {
  error_ = "notify backend requires Linux";
  return -kExitUnavailable;
}
} // namespace snowglobe::tracer
#else

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <map>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../redact/redact.hpp"
#include "../../util/string_util.hpp"
#include "../common/trace_helpers.hpp"
#include "../ptrace/open_flags.hpp"
#include "notify_filter.hpp"

// UAPI fallbacks for older libc headers (values verified against
// <linux/seccomp.h> via probe: RET_USER_NOTIF=0x7fc00000,
// NEW_LISTENER=0x8, CONTINUE=0x1, RECV=0xc0502100, SEND=0xc0182101,
// ID_VALID=0x40082102; sizeof notif/resp/data = 80/24/64).
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL (1 << 20)
#endif
#ifndef PTRACE_EVENT_FORK
#define PTRACE_EVENT_FORK 1
#endif
#ifndef PTRACE_EVENT_VFORK
#define PTRACE_EVENT_VFORK 2
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif
#ifndef PTRACE_EVENT_EXEC
#define PTRACE_EVENT_EXEC 4
#endif
#ifndef PTRACE_EVENT_VFORK_DONE
#define PTRACE_EVENT_VFORK_DONE 5
#endif
#ifndef PTRACE_EVENT_EXIT
#define PTRACE_EVENT_EXIT 6
#endif

namespace snowglobe::tracer {
namespace {

// x32 ABI high bit (R5 class): stripped before classifying so x32 stays
// observed. Matches the filter's JGE trap rule.
constexpr uint64_t kX32Bit = 0x40000000U;

std::atomic<int> gStop{0};
void onSignal(int) {
  gStop.fetch_add(1);
}

struct NotifyProc {
  pid_t ppid = -1;
  pid_t tgid = -1; // thread-group id (== tid for leaders)
  bool isThread = false;
};

bool sendListener(int sock, int fd, std::string& error) {
  char hello = 'O';
  struct msghdr msg = {};
  struct iovec iov = {&hello, 1};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  char cmsgBuf[CMSG_SPACE(sizeof(int))];
  msg.msg_control = cmsgBuf;
  msg.msg_controllen = sizeof(cmsgBuf);
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  if (::sendmsg(sock, &msg, 0) != 1) {
    error = std::string("send listener: ") + ::strerror(errno);
    return false;
  }
  return true;
}

bool recvListener(int sock, int& fdOut, std::string& error) {
  // Protocol: child sends "OK" + listener fd (SCM_RIGHTS), or
  // "E" + human text when filter install failed (then it exits 69).
  char buf[512] = {};
  struct msghdr msg = {};
  struct iovec iov = {buf, sizeof(buf) - 1};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  char cmsgBuf[CMSG_SPACE(sizeof(int))];
  msg.msg_control = cmsgBuf;
  msg.msg_controllen = sizeof(cmsgBuf);
  ssize_t n = 0;
  for (;;) {
    n = ::recvmsg(sock, &msg, 0);
    if (n >= 0) {
      break;
    }
    if (errno == EINTR) {
      continue; // ms-scale window; the child always answers promptly
    }
    error = std::string("handshake recv: ") + ::strerror(errno);
    return false;
  }
  if (n == 0) {
    error = "tracee died before handing over the notify fd";
    return false;
  }
  if (buf[0] == 'E') {
    error = "tracee filter install failed: " + std::string(buf + 1, static_cast<size_t>(n) - 1);
    return false;
  }
  if (buf[0] != 'O') {
    error = "tracee handshake garbled";
    return false;
  }
  for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
      std::memcpy(&fdOut, CMSG_DATA(cmsg), sizeof(fdOut));
      return true;
    }
  }
  error = "tracee handshake carried no fd";
  return false;
}

// Install the notify filter in the CALLER (the tracee child, pre-exec).
// R6: the child is single-threaded here (just forked), so plain
// seccomp(2) covers the only thread — never call this after threads
// exist without SECCOMP_FILTER_FLAG_TSYNC (silent fail-open on
// siblings). NO_NEW_PRIVS first (unprivileged install requirement);
// the filter inherits across the coming exec.
bool installNotifyFilter(int& listenerOut, std::string& error) {
  const std::vector<struct sock_filter> prog = buildNotifyFilter();
  if (prog.empty()) {
    error = "notify: seccomp unsupported on this arch";
    return false;
  }
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    error = std::string("notify: no_new_privs: ") + ::strerror(errno);
    return false;
  }
  struct sock_fprog fprog = {};
  fprog.len = static_cast<unsigned short>(prog.size());
  // seccomp() reads the program synchronously; the vector outlives it.
  fprog.filter = const_cast<struct sock_filter*>(prog.data());
  const long fd =
      ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &fprog);
  if (fd < 0) {
    error = std::string("notify: seccomp filter: ") + ::strerror(errno);
    return false;
  }
  listenerOut = static_cast<int>(fd);
  return true;
}

bool notifIdValid(int listener, uint64_t id) {
  // Takes a pointer to the id; 0 = still valid, -ENOENT = tracee gone.
  const long r = ::ioctl(listener, SECCOMP_IOCTL_NOTIF_ID_VALID, &id);
  if (r == 0) {
    return true;
  }
  return false;
}

bool notifRecv(int listener, struct seccomp_notif& req, bool& intrOut) {
  intrOut = false;
  if (::ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req) == 0) {
    return true;
  }
  // EINTR (supervisor signal) is a spurious wakeup: the main loop
  // processes pending signals and re-polls. Anything else is fatal —
  // a blind retry here would spin forever on a dead listener.
  intrOut = (errno == EINTR);
  return false;
}

// Exactly one SEND per valid notification. ENOENT (tracee died mid-answer)
// is benign; anything else is a supervisor failure (loud — a hung tracee
// would otherwise wedge, and the run tail SIGKILLs leftovers).
bool notifSend(int listener, uint64_t id, std::string& error) {
  struct seccomp_notif_resp resp = {};
  resp.id = id;
  resp.error = 0;
  resp.val = 0;
  resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
  for (;;) {
    if (::ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) == 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == ENOENT) {
      return true; // tracee gone; nothing left to answer
    }
    error = std::string("notify: answer send: ") + ::strerror(errno);
    return false;
  }
}

} // namespace

NotifyTracer* NotifyTracer::create() {
  return new NotifyTracer();
}

bool NotifyTracer::probeAvailable(std::string& reason) {
#ifdef SYS_seccomp
  unsigned int act = SECCOMP_RET_USER_NOTIF;
  errno = 0;
  const long r = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
  if (r == 0) {
    reason.clear();
    return true;
  }
  if (errno == EINVAL) {
    reason = "SECCOMP_RET_USER_NOTIF unknown (kernel < 5.11)";
  } else if (errno == ENOSYS) {
    reason = "no seccomp(2)";
  } else {
    reason = std::strerror(errno);
  }
  return false;
#else
  reason = "no SYS_seccomp for this arch";
  return false;
#endif
}

int NotifyTracer::run(const std::vector<std::string>& argv, const TraceOptions& opts) {
  using util::jsonEscape;
  if (argv.empty()) {
    error_ = "no command";
    return -kExitUsage;
  }
  if (opts.isolate) {
    // Non-isolate parity first: the isolate middle would need its own
    // listener hand-over (issue #6; documented in ADR-0009 follow-ups).
    // Degrade with the honest reason, never a half-trace.
    error_ = "notify backend with --isolate is not yet supported (use --backend=ptrace)";
    return -kExitUnavailable;
  }
  std::string reason;
  if (!probeAvailable(reason)) {
    error_ = "notify backend unavailable: " + reason;
    return -kExitUnavailable;
  }
  const bool allOpens = opts.allOpens;
  const std::vector<std::pair<std::string, std::string>> secretEnv = opts.secretEnv;

  struct timespec ts0 = {};
  clock_gettime(CLOCK_MONOTONIC, &ts0);
  const uint64_t tStartMs =
      static_cast<uint64_t>(ts0.tv_sec) * 1000ULL + static_cast<uint64_t>(ts0.tv_nsec) / 1000000ULL;
  auto nowTms = [&]() -> uint64_t {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL - tStartMs;
  };
  auto nowUs = []() -> uint64_t { return clockUs(CLOCK_REALTIME); };
  auto emitEv = [&](const std::string& body) -> bool { return emit(body); };

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

  // Listener hand-over socket (CLOEXEC both ends: nothing leaks into the
  // agent image; the listener itself travels via SCM_RIGHTS).
  int sv[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
    error_ = std::string("notify: socketpair: ") + ::strerror(errno);
    sigaction(SIGINT, &oldInt, nullptr);
    sigaction(SIGTERM, &oldTerm, nullptr);
    return -kExitSoftware;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    error_ = std::string("fork: ") + errnoText(errno);
    ::close(sv[0]);
    ::close(sv[1]);
    sigaction(SIGINT, &oldInt, nullptr);
    sigaction(SIGTERM, &oldTerm, nullptr);
    return -kExitSoftware;
  }
  if (child == 0) {
    // --- tracee: stop for the seize, install the filter, hand over the
    // listener, exec. Straight-line code: no trapped syscalls before the
    // filter is up (seccomp/sendmsg/close/exec only after install... and
    // execve IS trapped, by design — the supervisor answers it).
    ::close(sv[0]);
    ::raise(SIGSTOP);
    int listener = -1;
    std::string instErr;
    if (!installNotifyFilter(listener, instErr)) {
      // Loud path: tell the supervisor (it reports EX_UNAVAILABLE),
      // then exit. A bare _exit would surface as a confusing agent-127.
      const std::string msg = "E" + instErr;
      const ssize_t wn = ::write(sv[1], msg.c_str(), msg.size());
      (void)wn; // best effort: the supervisor treats short/failed
                // handshakes as EX_UNAVAILABLE either way
      _exit(69);
    }
    std::string sendErr;
    if (!sendListener(sv[1], listener, sendErr)) {
      _exit(69);
    }
    ::close(sv[1]);
    // The supervisor's copy keeps the listener alive; drop ours so the
    // agent image carries no supervisor fd (plain fd refcounting — the
    // supervisor holds its copy from hand-over until the tree is reaped).
    ::close(listener);
    ::setenv("UV_USE_IO_URING", "0", 0);
    ::execvp(cargv[0], cargv.data());
    _exit(127); // exec failed; supervisor recorded N attempts + exit 127
  }
  ::close(sv[1]);

  // --- supervisor ---
  std::map<pid_t, NotifyProc> procs;
  procs[child].ppid = ::getpid();
  procs[child].tgid = child; // root starts single-threaded
  int finalCode = 0;
  bool finalSet = false;
  bool dead = false;
  int stopCount = 0;
  int listener = -1;

  // Cleanup helper for every early return below (restores handlers once).
  bool handlersRestored = false;
  auto restoreHandlers = [&]() {
    if (!handlersRestored) {
      handlersRestored = true;
      sigaction(SIGINT, &oldInt, nullptr);
      sigaction(SIGTERM, &oldTerm, nullptr);
    }
  };

  // Cleanup for every post-fork early return: the child may be
  // SIGSTOPped and (if seize failed) untraced — SIGKILL + blocking reap
  // so no stopped orphan survives us. Fds closed, handlers restored.
  auto failChild = [&](const std::string& msg, int code) -> int {
    error_ = msg;
    ::kill(child, SIGKILL);
    int st = 0;
    ::waitpid(child, &st, 0);
    ::close(sv[0]);
    restoreHandlers();
    return code;
  };

  // Wait for the root's self-stop, then seize: lifecycle arrives via
  // SEIZE + TRACE* events from here on (no syscall stops, ever). WUNTRACED
  // is load-bearing here: the child is not traced yet, and untraced stops
  // are invisible to waitpid without it (proven by probe/wuntraced.c —
  // without the flag this wait hangs forever while the child sits in
  // do_signal_stop). Post-seize every descendant is traced, whose stops
  // are always reported.
  {
    int status = 0;
    for (;;) {
      const pid_t w = ::waitpid(child, &status, __WALL | WUNTRACED);
      if (w < 0) {
        if (errno == EINTR) {
          continue;
        }
        return failChild(std::string("notify: initial waitpid: ") + errnoText(errno),
                         -kExitSoftware);
      }
      if (WIFEXITED(status) || WIFSIGNALED(status)) {
        return failChild("tracee died before seize (external SIGKILL in the fork window?)",
                         -kExitSoftware);
      }
      if (WIFSTOPPED(status)) {
        break;
      }
    }
    if (::ptrace(PTRACE_SEIZE, child, nullptr, nullptr) != 0) {
      return failChild(std::string("notify: seize: ") + errnoText(errno), -kExitSoftware);
    }
    const long kTraceOpts = PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
                            PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT | PTRACE_O_EXITKILL;
    if (::ptrace(PTRACE_SETOPTIONS, child, nullptr, reinterpret_cast<void*>(kTraceOpts)) != 0) {
      return failChild(std::string("notify: setoptions: ") + errnoText(errno), -kExitSoftware);
    }
    if (::ptrace(PTRACE_CONT, child, nullptr, nullptr) != 0) {
      return failChild(std::string("notify: initial resume: ") + errnoText(errno), -kExitSoftware);
    }
  }

  // Listener hand-over (blocking: the child sends before exec, so this
  // cannot hang unless the child died — recvmsg then reports EOF).
  {
    std::string hsErr;
    if (!recvListener(sv[0], listener, hsErr)) {
      return failChild(std::string("notify: ") + hsErr, -kExitUnavailable);
    }
  }
  ::close(sv[0]);

  // run.meta (cmd redacted like proc.exec argv — ADR-0003) + root start.
  // backend:notify is additive (schema 0); the normaliser strips it.
  {
    std::string cmdJson;
    for (const auto& a : argv) {
      if (!cmdJson.empty()) {
        cmdJson += ",";
      }
      cmdJson += jsonEscape(redact::redactText(a, secretEnv));
    }
    char cwd[4096] = {};
    std::string cwdStr;
    if (::getcwd(cwd, sizeof(cwd)) != nullptr) {
      cwdStr = cwd;
    }
    emitEv("{\"ts_us\":" + std::to_string(nowUs()) + ",\"t_ms\":" + std::to_string(nowTms()) +
           ",\"ev\":\"run.meta\",\"pid\":" + std::to_string(child) +
           ",\"tid\":" + std::to_string(child) + ",\"cmd\":[" + cmdJson +
           "],\"cwd\":" + jsonEscape(cwdStr) + ",\"backend\":\"notify\"}");
    emitEv("{\"ts_us\":" + std::to_string(nowUs()) + ",\"t_ms\":" + std::to_string(nowTms()) +
           ",\"ev\":\"proc.start\",\"pid\":" + std::to_string(child) + ",\"tid\":" +
           std::to_string(child) + ",\"ppid\":" + std::to_string(::getpid()) + ",\"root\":true}");
  }

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

  // Decode one notification (entry-only) and emit its event. The
  // caller SENDs the answer either way — a valid notification is never
  // left unanswered. Entry-known filters (O_DIRECTORY/O_PATH, noisy
  // paths) apply exactly like the ptrace backend; the failure-based
  // read-open filter cannot apply (outcome unknown) — failed probes to
  // non-noisy paths appear with result_known:false. Documented delta,
  // pinned by goldens-notify.
  auto handleNotif = [&](pid_t pid, uint64_t rawNr, const uint64_t args[6]) {
    if (procs.find(pid) == procs.end()) {
      procs[pid].ppid = -1; // fork-race: reconcile at the parent's event
    }
    uint64_t nr = rawNr;
    if ((nr & kX32Bit) != 0) {
      nr &= ~kX32Bit; // x32 shares the arch: strip the bit, stay observed
    }
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
    if (kind == Kind::None) {
      return; // x32 tail the JGE trapped but we decode nothing:
              // continue silently, like the ptrace backend ignores
              // syscalls outside its decode set.
    }
    const std::string ts = tsIds(pid);
    // result_known:false marks every event whose outcome the ptrace
    // backend would know from the syscall exit (additive, schema 0).
    const char* unknown = ",\"result_known\":false}";
    switch (kind) {
    case Kind::Exec: {
      // Attempt semantics: emitted at allow time. Success and failure
      // are indistinguishable at entry (no exit stops), so failed execs
      // surface as proc.exec with result_known:false — never as
      // proc.exec_failed (no errno exists). Pinned by goldens-notify.
      const bool isAt =
#ifdef SYS_execveat
          (nr == static_cast<uint64_t>(SYS_execveat));
#else
          false;
#endif
      const uint64_t pathAddr = isAt ? args[1] : args[0];
      const uint64_t argvAddr = isAt ? args[2] : args[1];
      const long dirfd = isAt ? static_cast<long>(args[0]) : AT_FDCWD;
      const std::string cwd = readLink("/proc/" + std::to_string(pid) + "/cwd");
      std::string detail, canon, argvJson;
      bool truncated = false;
      if (!readExecStrings(pid, dirfd, pathAddr, argvAddr, secretEnv, canon, argvJson, detail,
                           truncated)) {
        emitDecodeError(pid, rawNr, "exec path: " + detail);
        break;
      }
      emitEv("{" + ts + ",\"ev\":\"proc.exec\",\"path\":" + jsonEscape(canon) + ",\"argv\":[" +
             argvJson + "],\"cwd\":" + jsonEscape(cwd) + (truncated ? ",\"truncated\":true" : "") +
             unknown);
      break;
    }
    case Kind::Open: {
      bool isCreat = false;
#ifdef SYS_creat
      isCreat = (nr == static_cast<uint64_t>(SYS_creat));
#endif
      bool isOpenat2 = false;
#ifdef SYS_openat2
      isOpenat2 = (nr == static_cast<uint64_t>(SYS_openat2));
#endif
      long dirfd = AT_FDCWD;
      uint64_t pathAddr = args[0];
      uint64_t flagArg = 0;
#ifdef SYS_openat
      if (nr == static_cast<uint64_t>(SYS_openat)) {
        dirfd = static_cast<long>(args[0]);
        pathAddr = args[1];
        flagArg = args[2];
      }
#endif
#ifdef SYS_open
      if (nr == static_cast<uint64_t>(SYS_open)) {
        flagArg = args[1];
      }
#endif
      if (isOpenat2) {
        dirfd = static_cast<long>(args[0]);
        pathAddr = args[1];
        // struct open_how starts with flags (__u64 at offset 0).
        if (!vmReadU64(pid, args[2], flagArg)) {
          emitDecodeError(pid, rawNr, "openat2 open_how: unreadable");
          break;
        }
      }
      const OpenFlags of = classifyOpenFlags(flagArg, isCreat);
      if (!allOpens && of.dirOrPath) {
        break; // default filter: O_DIRECTORY / O_PATH carry no content
      }
      std::string path, detail;
      if (!vmReadStr(pid, pathAddr, path, detail)) {
        emitDecodeError(pid, rawNr, "open path: " + detail);
        break;
      }
      const std::string canon = canonicalPath(pid, dirfd, path);
      if (!allOpens && !of.write && isNoisyPath(canon)) {
        break; // default filter: noisy read-opens. The failure-based
               // half of the ptrace filter cannot apply (no exit):
               // failed probes to non-noisy paths are recorded unknown.
      }
      // No fd key (unknown at entry) and no ok/errno: result_known:false.
      emitEv("{" + ts + ",\"ev\":\"fs.open\",\"path\":" + jsonEscape(canon) + ",\"write\":" +
             (of.write ? "true" : "false") + ",\"create\":" + (of.create ? "true" : "false") +
             ",\"trunc\":" + (of.trunc ? "true" : "false") +
             (of.tmpfile ? ",\"tmpfile\":true" : "") + unknown);
      break;
    }
    case Kind::Unlink: {
      bool isAt = false;
#ifdef SYS_unlinkat
      isAt = (nr == static_cast<uint64_t>(SYS_unlinkat));
#endif
      long dirfd = AT_FDCWD;
      uint64_t pathAddr = args[0];
      if (isAt) {
        dirfd = static_cast<long>(args[0]);
        pathAddr = args[1];
      }
      bool rmdir = false;
#ifdef SYS_rmdir
      rmdir = (nr == static_cast<uint64_t>(SYS_rmdir));
#endif
#ifdef AT_REMOVEDIR
      rmdir = rmdir || (isAt && ((args[2] & AT_REMOVEDIR) != 0));
#endif
      std::string path, detail;
      if (!vmReadStr(pid, pathAddr, path, detail)) {
        emitDecodeError(pid, rawNr, "unlink path: " + detail);
        break;
      }
      emitEv("{" + ts + ",\"ev\":\"" + (rmdir ? "fs.rmdir" : "fs.unlink") +
             "\",\"path\":" + jsonEscape(canonicalPath(pid, dirfd, path)) + unknown);
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
      uint64_t fromAddr = args[0], toAddr = args[1];
      if (twoDir) {
        fromDir = static_cast<long>(args[0]);
        fromAddr = args[1];
        toDir = static_cast<long>(args[2]);
        toAddr = args[3];
      }
      std::string from, to, d;
      if (!vmReadStr(pid, fromAddr, from, d)) {
        emitDecodeError(pid, rawNr, "rename from: " + d);
        break;
      }
      if (!vmReadStr(pid, toAddr, to, d)) {
        emitDecodeError(pid, rawNr, "rename to: " + d);
        break;
      }
      emitEv("{" + ts +
             ",\"ev\":\"fs.rename\",\"from\":" + jsonEscape(canonicalPath(pid, fromDir, from)) +
             ",\"to\":" + jsonEscape(canonicalPath(pid, toDir, to)) + unknown);
      break;
    }
    case Kind::Mkdir: {
      // No outcome key in either backend: identical shape, no result_known.
      bool isAt = false;
#ifdef SYS_mkdirat
      isAt = (nr == static_cast<uint64_t>(SYS_mkdirat));
#endif
      long dirfd = AT_FDCWD;
      uint64_t pathAddr = args[0];
      if (isAt) {
        dirfd = static_cast<long>(args[0]);
        pathAddr = args[1];
      }
      std::string path, detail;
      if (!vmReadStr(pid, pathAddr, path, detail)) {
        emitDecodeError(pid, rawNr, "mkdir path: " + detail);
        break;
      }
      emitEv("{" + ts +
             ",\"ev\":\"fs.mkdir\",\"path\":" + jsonEscape(canonicalPath(pid, dirfd, path)) + "}");
      break;
    }
    case Kind::Symlink: {
      bool isAt = false;
#ifdef SYS_symlinkat
      isAt = (nr == static_cast<uint64_t>(SYS_symlinkat));
#endif
      uint64_t targetAddr = args[0];
      long dirfd = AT_FDCWD;
      uint64_t linkAddr = args[1];
      if (isAt) {
        dirfd = static_cast<long>(args[1]);
        linkAddr = args[2];
      }
      std::string target, link, d;
      if (!vmReadStr(pid, targetAddr, target, d)) {
        emitDecodeError(pid, rawNr, "symlink target: " + d);
        break;
      }
      if (!vmReadStr(pid, linkAddr, link, d)) {
        emitDecodeError(pid, rawNr, "symlink path: " + d);
        break;
      }
      emitEv("{" + ts + ",\"ev\":\"fs.symlink\",\"target\":" + jsonEscape(target) +
             ",\"path\":" + jsonEscape(canonicalPath(pid, dirfd, link)) + unknown);
      break;
    }
    case Kind::Chmod: {
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
        const long fd = static_cast<long>(args[0]);
        mode = args[1];
        path = readLink("/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd));
        if (path.empty()) {
          emitDecodeError(pid, rawNr, "fchmod fd has no path");
          break;
        }
        path = normaliseAbs(path);
      } else {
        long dirfd = AT_FDCWD;
        uint64_t pathAddr = args[0];
        mode = args[1];
        if (isAt) {
          dirfd = static_cast<long>(args[0]);
          pathAddr = args[1];
          mode = args[2];
        }
        std::string raw, detail;
        if (!vmReadStr(pid, pathAddr, raw, detail)) {
          emitDecodeError(pid, rawNr, "chmod path: " + detail);
          break;
        }
        path = canonicalPath(pid, dirfd, raw);
      }
      // Mode is an octal string ("0755"): stable across readers, no
      // decimal/octal ambiguity in goldens.
      char modeStr[16] = {};
      std::snprintf(modeStr, sizeof(modeStr), "0%o", (unsigned int)mode);
      emitEv("{" + ts + ",\"ev\":\"fs.chmod\",\"path\":" + jsonEscape(path) +
             ",\"mode\":" + jsonEscape(modeStr) + unknown);
      break;
    }
    case Kind::Connect:
    case Kind::Sendto:
    case Kind::Bind: {
      uint64_t addrArg = args[1];
      uint64_t lenArg = args[2];
      if (kind == Kind::Sendto) {
        addrArg = args[4];
        lenArg = args[5];
      }
      const char* evName = kind == Kind::Connect
                               ? "net.connect"
                               : (kind == Kind::Sendto ? "net.sendto" : "net.bind");
      if (addrArg == 0 || lenArg == 0) {
        if (kind == Kind::Connect) {
          emitEv("{" + ts + ",\"ev\":\"net.connect\",\"family\":\"unknown\"" + unknown);
        }
        break; // sendto/bind with no peer: nothing to record (as ptrace)
      }
      if (lenArg > 256) {
        emitDecodeError(pid, rawNr, "sockaddr too long");
        break;
      }
      char sbuf[256] = {};
      const std::size_t wantLen = static_cast<std::size_t>(lenArg) > sizeof(sbuf)
                                      ? sizeof(sbuf)
                                      : static_cast<std::size_t>(lenArg);
      if (vmRead(pid, addrArg, sbuf, wantLen) < 0) {
        emitDecodeError(pid, rawNr, std::string("sockaddr: ") + errnoText(errno));
        break;
      }
      const util::SockaddrParts parts =
          util::parseSockaddr(sbuf, static_cast<unsigned long>(wantLen));
      // AF_UNSPEC connect() is the UDP-disconnect idiom: no peer, so
      // net.disconnect — outcome unknown at entry.
      if (kind == Kind::Connect && parts.family == "unspec") {
        emitEv("{" + ts + ",\"ev\":\"net.disconnect\"" + unknown);
        break;
      }
      std::string endpoint =
          "\"family\":" + jsonEscape(parts.family) + ",\"addr\":" + jsonEscape(parts.display);
      if (parts.family == "ipv4" || parts.family == "ipv6") {
        endpoint += ",\"ip\":" + jsonEscape(parts.ip) + ",\"port\":" + std::to_string(parts.port);
      } else if (parts.family == "unix") {
        endpoint += ",\"path\":" + jsonEscape(parts.path);
      }
      emitEv("{" + ts + ",\"ev\":\"" + evName + "\"," + endpoint + unknown);
      break;
    }
    case Kind::None: break;
    }
  };

  // Drain all pending waitpid events (WNOHANG): fork/clone/exec/exit
  // lifecycle. Returns false when the run must end (error_ set) or the
  // tree drained (dead set).
  auto drainWait = [&]() -> bool {
    for (;;) {
      int status = 0;
      // WUNTRACED: harmless once all descendants are traced (their stops
      // report regardless), load-bearing if any stop ever arrives untraced.
      const pid_t pid = ::waitpid(-1, &status, __WALL | WNOHANG | WUNTRACED);
      if (pid < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == ECHILD) {
          // No waitable tracees left but entries remain (threads that
          // vanished in an exec race we never saw): close them out
          // honestly rather than hanging or dropping them silently.
          for (const auto& [tid, _] : procs) {
            emitVanished(tid);
          }
          procs.clear();
          dead = true;
          return true;
        }
        error_ = std::string("waitpid: ") + errnoText(errno);
        finalCode = kExitSoftware;
        finalSet = true;
        return false;
      }
      if (pid == 0) {
        return true; // nothing pending
      }
      if (procs.find(pid) == procs.end()) {
        procs[pid].ppid = -1; // fork-race: reconciled at the parent event
      }
      if (WIFEXITED(status) || WIFSIGNALED(status)) {
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        const int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        emitEv("{" + tsIds(pid) + ",\"ev\":\"proc.exit\",\"code\":" + std::to_string(code) +
               ",\"signal\":" + std::to_string(sig) + "}");
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

      if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK ||
          event == PTRACE_EVENT_CLONE) {
        unsigned long msg = 0;
        if (::ptrace(PTRACE_GETEVENTMSG, pid, nullptr, &msg) != 0) {
          // ESRCH: tracee died mid-event; its exit will be reaped next.
          ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
          continue;
        }
        const auto cpid = static_cast<pid_t>(msg);
        auto& ci = procs[cpid]; // reconciles fork-race pending entries
        ci.ppid = pid;
        const pid_t tgid = threadGroupId(cpid);
        ci.tgid = tgid;
        ci.isThread = (tgid != cpid);
        emitEv("{" + tsIds(cpid) + ",\"ev\":\"proc.start\",\"ppid\":" + std::to_string(pid) +
               (ci.isThread ? ",\"thread\":true}" : "}"));
        ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
        continue;
      }
      if (event == PTRACE_EVENT_EXEC) {
        // Siblings of the old image may vanish with no exit stop:
        // close out anything still tracked under the old tgid. (No
        // entry-cache scavenge here: proc.exec was already emitted at
        // allow time with result_known:false.)
        NotifyProc& ex = procs[pid];
        const pid_t oldTgid = ex.tgid < 0 ? pid : ex.tgid;
        std::vector<pid_t> gone;
        for (const auto& [tid, info] : procs) {
          if (tid != pid && info.tgid == oldTgid) {
            gone.push_back(tid);
          }
        }
        for (const pid_t t : gone) {
          emitVanished(t);
          procs.erase(t);
        }
        ex.tgid = pid;
        ex.isThread = false;
        ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
        continue;
      }
      if (event == PTRACE_EVENT_EXIT || event == PTRACE_EVENT_VFORK_DONE) {
        ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
        continue;
      }
      // Resume strategy mirrors the ptrace backend with CONT instead of
      // SYSCALL (no syscall stops exist here): event stops and SIGSTOP
      // noise resume bare; stray SIGTRAP resumes bare; real signals are
      // forwarded. SIGCHLD is always genuine delivery: forward it.
      if (sig == SIGSTOP || event != 0 || sig == SIGTRAP) {
        ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
      } else {
        ::ptrace(PTRACE_CONT, pid, nullptr, reinterpret_cast<void*>(static_cast<long>(sig)));
      }
    }
  };

  // Main loop: drain lifecycle promptly, then poll the listener.
  // The 1 ms poll cap bounds quiet-event latency: waitpid stops that
  // arrive with no concurrent notification (fork-event resumes when the
  // child is slow to exec, exit reaps) must not sleep a full scheduler
  // quantum — measured: 20 ms here cost 65x on forkexec-300 (each quiet
  // event stalled the whole timeout); 1 ms costs ~1000 cheap wakeups/s
  // and keeps fork latency notification-driven (bench/results/).
  struct pollfd pfd = {};
  pfd.fd = listener;
  pfd.events = POLLIN;
  while (!dead) {
    // Supervisor signals, counted so two rapid SIGINTs don't coalesce
    // (second Ctrl-C means SIGKILL) — same contract as ptrace.
    const int pending = gStop.exchange(0);
    if (pending > 0) {
      stopCount += pending;
      if (stopCount == 1) {
        if (procs.find(child) != procs.end()) {
          ::kill(child, SIGTERM);
        } else {
          for (const auto& [tid, _] : procs) {
            ::kill(tid, SIGTERM);
          }
        }
      } else {
        for (const auto& [tid, _] : procs) {
          ::kill(tid, SIGKILL);
        }
        error_ = "interrupted";
        break;
      }
    }
    if (!drainWait()) {
      break;
    }
    if (dead) {
      break;
    }
    const int pr = ::poll(&pfd, 1, 1);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      error_ = std::string("notify: poll: ") + errnoText(errno);
      break;
    }
    if (pr == 0) {
      continue; // timeout: loop back to drain/reap
    }
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
      error_ = "notify: listener error (POLLERR/POLLNVAL — internal bug, report it)";
      break;
    }
    if ((pfd.revents & POLLIN) == 0) {
      continue;
    }
    struct seccomp_notif req = {};
    bool intr = false;
    if (!notifRecv(listener, req, intr)) {
      if (intr) {
        continue; // supervisor signal: process it at the loop top
      }
      error_ = "notify: recv failed (listener lost?)";
      break;
    }
    // Validate before every read: the tracee may have died between
    // POLLIN and RECV. Stale ids are skipped (kernel already dropped
    // them) — never answered twice, never answered dead.
    if (!notifIdValid(listener, req.id)) {
      continue;
    }
    const pid_t npid = static_cast<pid_t>(req.pid);
    uint64_t args[6] = {};
    for (int i = 0; i < 6; ++i) {
      args[i] = req.data.args[i];
    }
    const uint64_t rawNr = static_cast<uint64_t>(static_cast<uint32_t>(req.data.nr));
    handleNotif(npid, rawNr, args);
    // Exactly one answer per valid notification — prompt, before any
    // slow work could follow. Never hold a blocked syscall hostage.
    std::string sendErr;
    if (!notifSend(listener, req.id, sendErr)) {
      error_ = sendErr;
      break;
    }
  }

  // No orphans: ensure everything is reaped/killed. EXITKILL already
  // covers supervisor death; this covers our own exits (Ctrl-C x2,
  // internal errors) — including tracees parked inside notifications.
  for (const auto& [pid, _] : procs) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, WNOHANG);
  }
  ::close(listener);

  restoreHandlers();
  if (!error_.empty()) {
    // Supervisor failure (interrupted, wait/poll error, seize/handshake
    // failure): loud EX_SOFTWARE with a partial manifest, never a fake
    // agent exit code. The CLI prints error_ in this path.
    return -kExitSoftware;
  }
  if (!finalSet) {
    error_ = "lost track of root exit status";
    return -kExitSoftware;
  }
  return finalCode;
}

} // namespace snowglobe::tracer
#endif // __linux__

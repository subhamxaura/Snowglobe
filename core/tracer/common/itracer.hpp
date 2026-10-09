#pragma once
// Thread ownership: implementations are driven from the supervisor thread.
//
// SyscallBackend interface (ADR-0009): every tracer backend implements
// ITracer and is constructed through createTracer(). PtraceTracer is the
// default (byte-identical semantics, forever); NotifyTracer (seccomp
// user-notification) is selected explicitly via --backend=notify and
// shares this interface so parity tests drive both backends identically.
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snowglobe::tracer {

// Exit-code conventions shared by all tracer backends.
constexpr int kExitUsage = 64;
constexpr int kExitUnavailable = 69;
constexpr int kExitSoftware = 70;

struct TraceOptions {
  bool allOpens = false;       // --all-opens: do not filter read-opens / noisy paths
  std::string tracer = "auto"; // --backend/--tracer selection (ADR-0009): "auto" (default,
                               // resolves to ptrace until the notify backend lands),
                               // "ptrace", "notify" ("seccomp" is accepted as an alias).
  // Sensitive (name, value) pairs for argv redaction (ADR-0003), collected
  // by the supervisor from its own environment after proxy env injection.
  std::vector<std::pair<std::string, std::string>> secretEnv;
  // --isolate (ADR-0007): the middle was spawned pre-threads by the
  // caller and handshaked (maps + setup status); the tracer adopts its
  // pid and starts the wait loop at its first SIGSTOP. -1 = fork here
  // (non-isolate path, unchanged).
  bool isolate = false;
  long long isolateChild = -1;
  // --isolate secret masks for run.meta (absolute masked paths, already
  // minus --allow-path exemptions). Empty when not isolating.
  std::vector<std::string> isolateMasks;
};

class ITracer {
public:
  virtual ~ITracer() = default;
  // Trace `argv` (argv[0] = program), emitting events via the sink as
  // JSON payloads *without* chain fields. Returns the child's exit code, or
  // a negative value + error() on supervisor failure.
  virtual int run(const std::vector<std::string>& argv, const TraceOptions& opts) = 0;
  virtual const std::string& error() const = 0;

  using EventSink = std::function<bool(const std::string& jsonWithoutChain)>;
  void setSink(EventSink sink) {
    sink_ = std::move(sink);
  }

protected:
  EventSink sink_;
  bool emit(const std::string& json) {
    if (sink_) {
      return sink_(json);
    }
    return true;
  }
};

// Backend selection (ADR-0009). Normalises aliases ("seccomp" → "notify")
// and resolves "auto" → "ptrace" (notify becomes the auto choice only
// after BLOCK 2 numbers + parity land). Returns nullptr + error on
// unknown names or on non-Linux platforms; the caller maps that to
// EX_UNAVAILABLE with the returned message.
std::unique_ptr<ITracer> createTracer(const std::string& backend, std::string& error);

} // namespace snowglobe::tracer

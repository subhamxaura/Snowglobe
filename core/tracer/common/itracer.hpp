#pragma once
// Thread ownership: implementations are driven from the supervisor thread.
#include <functional>
#include <string>
#include <vector>

namespace snowglobe::tracer {

// Exit-code conventions shared by all tracer backends.
constexpr int kExitUsage = 64;
constexpr int kExitUnavailable = 69;
constexpr int kExitSoftware = 70;

struct TraceOptions {
  bool allOpens = false;  // --all-opens: do not filter read-opens / noisy paths
  std::string tracer = "auto";
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

}  // namespace snowglobe::tracer

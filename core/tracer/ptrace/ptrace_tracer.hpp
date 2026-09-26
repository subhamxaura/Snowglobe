#pragma once
#include "../common/itracer.hpp"

#include <cstdint>
#include <string>

namespace snowglobe::tracer {

// ptrace backend (AGENTS.md §1.3 v0.1).
// Linux-only; on other platforms `create()` returns nullptr.
class PtraceTracer : public ITracer {
 public:
  static PtraceTracer* create();
  int run(const std::vector<std::string>& argv, const TraceOptions& opts) override;
  const std::string& error() const override { return error_; }

 private:
  std::string error_;
  bool allOpens_ = false;
  uint64_t tStartMs_ = 0;

  uint64_t nowTms() const;
  uint64_t nowUs() const;
  bool emitEv(const std::string& body);
};

}  // namespace snowglobe::tracer

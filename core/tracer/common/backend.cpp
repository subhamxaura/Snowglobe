// SyscallBackend factory (ADR-0009): one selection point for both tracer
// backends. Thread ownership: called once from the supervisor thread.
#include "itracer.hpp"

#include "../notify/notify_tracer.hpp"
#include "../ptrace/ptrace_tracer.hpp"

#include <memory>
#include <string>

namespace snowglobe::tracer {

std::unique_ptr<ITracer> createTracer(const std::string& backend, std::string& error) {
  std::string name = backend;
  if (name == "seccomp") {
    name = "notify"; // historical alias (AGENTS.md §2 used to say seccomp)
  }
  if (name == "auto" || name.empty()) {
    name = "ptrace"; // notify becomes the auto choice only after BLOCK 2
  }
  if (name == "ptrace") {
    PtraceTracer* t = PtraceTracer::create();
    if (t == nullptr) {
      error = "ptrace backend requires Linux";
      return nullptr;
    }
    return std::unique_ptr<ITracer>(t);
  }
  if (name == "notify") {
    NotifyTracer* t = NotifyTracer::create();
    if (t == nullptr) {
      error = "notify backend requires Linux";
      return nullptr;
    }
    return std::unique_ptr<ITracer>(t);
  }
  error = "unknown backend '" + backend + "' (want auto|ptrace|notify)";
  return nullptr;
}

} // namespace snowglobe::tracer

#pragma once
// Format-agnostic LLM recording proxy: base-URL injection target that stores
// raw request/response bytes plus per-chunk timing, and emits llm.request /
// llm.response events. Provider parsing lives in the TypeScript viewer;
// C++ extracts only top-level "model"/"stream" (see model_scan.hpp).
//
// Thread ownership: the httplib server runs its pool; each request spawns
// one upstream worker. Shared per-request state lives in a shared_ptr
// (joined before the provider returns — never detached). The event sink is
// called from handler threads and MUST be mutex-guarded by the owner (the
// CLI wraps it). All atomics/mutexes documented for TSan-cleanliness.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "../tracer/common/itracer.hpp"
#include "blocking_queue.hpp"
#include "model_scan.hpp"

namespace snowglobe::proxy {

struct ProxyOptions {
  std::string runDir; // <run>.sgr (llm/ created inside)
  // Provider -> base URL overrides (--upstream PROVIDER=URL). Keys lowercased.
  std::map<std::string, std::string> upstream;
  tracer::ITracer::EventSink sink; // MUST be thread-safe (CLI mutex-guards it)
  long connectTimeoutS = 30;
  long idleTimeoutS = 120; // idle between bytes; no total cap (streams run minutes)
};

class LlmProxy {
public:
  explicit LlmProxy(ProxyOptions opts);
  ~LlmProxy();

  LlmProxy(const LlmProxy&) = delete;
  LlmProxy& operator=(const LlmProxy&) = delete;

  // Bind 127.0.0.1:<ephemeral> and start serving. Returns the port, or -1
  // with error() set.
  int start();
  // Stop accepting, drain in-flight streams up to deadlineS, abort anything
  // still live past the deadline, join all threads, then return. Worst case
  // is one idle timeout for a read that stalled exactly across shutdown.
  void stop(long deadlineS = 10);
  int port() const {
    return port_;
  }
  // Completed 2xx responses ("turns" for the run epilogue).
  long turns() const {
    return turns_.load();
  }
  const std::string& error() const {
    return error_;
  }

private:
  // Per-request shared state. The handler thread creates it; the upstream
  // worker and the chunked provider share it. The provider ALWAYS joins the
  // worker before its final return, so no thread is ever detached and TSan
  // sees only mutex/atomic synchronization.
  struct Flight {
    BlockingQueue respQ{1 << 20};
    std::mutex mu;
    std::condition_variable cv;
    bool headDone = false;
    bool headError = false;
    std::string headErrText;
    int status = 0;
    httplib::Headers resHeaders;
    std::string resContentType;
    std::string resExt = ".bin";
    std::atomic<bool> abort{false};
    std::thread upThread;
    // Recording: upstream thread writes; server thread reads after join().
    std::string resPath, idxPath;
    std::ofstream resFile, idxFile;
    uint64_t bytes = 0, chunks = 0;
    uint64_t tStartWall = 0, tStartMono = 0, ttfbWall = 0;
    bool haveTtfb = false, upError = false;
  };

  struct Route {
    bool ok = false;
    std::string provider; // openai|anthropic|gemini|custom|unknown
    std::string base;     // upstream base URL (no trailing /)
    std::string path;     // upstream path+query
  };
  Route resolve(const std::string& target) const;

  // Handler entry points (run on httplib pool threads).
  void handleBody(const httplib::Request& req, httplib::Response& res,
                  const httplib::ContentReader& reader);
  void handlePlain(const httplib::Request& req, httplib::Response& res);
  void emitRequestEvent(long id, const std::string& provider, const std::string& method,
                        const std::string& path, const JsonTop& jt, uint64_t bytes, uint64_t tsUs,
                        pid_t tid);
  void emitResponseEvent(long id, int status, uint64_t bytes, bool hasTtfb, uint64_t ttfbMs,
                         uint64_t totalMs, uint64_t chunks, bool truncated,
                         const std::string& reqRel, const std::string& resRel,
                         const std::string& idxRel, uint64_t tsUs, pid_t tid);
  // t_ms since proxy start (≈ run start; see handleBody note).
  uint64_t tMs() const;

  ProxyOptions opts_;
  httplib::Server svr_;
  std::thread serveThread_;
  std::thread stopThread_;
  std::mutex stopMu_;
  bool stopStarted_ = false;
  std::atomic<bool> shuttingDown_{false};
  std::atomic<long> nextId_{0};
  std::atomic<long> turns_{0};
  uint64_t tEpochMonoMs_ = 0;
  // Live in-flight requests (for abort-on-deadline at stop()).
  std::mutex liveMu_;
  std::set<std::shared_ptr<Flight>> live_;
  int port_ = -1;
  std::string error_;
};

} // namespace snowglobe::proxy

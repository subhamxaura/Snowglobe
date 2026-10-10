#pragma once
// Recorded-run loader + replay matcher (ADR-0010).
//
// A ReplayStore loads one original run's LLM layer: every llm.response
// event in events.jsonl (id/status/req/res/idx) joined to its stored
// request envelope (method/path/provider/body, base64-decoded when
// wrapped) and its response blob pre-sliced at the recorded .idx chunk
// boundaries. match() implements the two-level rule (hash primary,
// same-endpoint sequential fallback), consumes the served turn, and on
// MISS appends to <orig-run>/replay.unrecorded.jsonl (loud, never
// invented). All response bodies are held in memory (mirrors the live
// proxy's per-request upload buffering; a 50 MB trace replays from RAM).
//
// Thread ownership: load() is single-threaded (startup); match() is
// mutex-guarded and safe from httplib handler threads. Counters are
// atomic for lock-free epilogue reads.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace snowglobe::replay {

struct RecordedTurn {
  long id = -1;         // NNNN from the original run
  std::string provider; // openai|anthropic|gemini|custom|unknown
  std::string method;
  std::string path;    // proxy path as recorded (e.g. /openai/v1/chat/...)
  std::string reqBody; // verbatim request bytes
  std::string hash;    // requestHash(provider, path, reqBody)
  std::string resExt;  // .json | .sse | .bin (drives replay Content-Type)
  int status = 0;      // recorded HTTP status, re-served verbatim
  std::string resBody; // full recorded response bytes
  std::string resSha256;
  std::vector<std::string> chunks; // resBody sliced at .idx bounds
  std::vector<uint64_t> gapsUs;    // inter-chunk ts_us gaps (for --realtime)
  bool used = false;               // consumed by exactly one serve
};

struct ReplayMatch {
  bool hit = false;
  bool primary = false; // true = hash match, false = endpoint-order fallback
  long recordedId = -1;
  size_t slot = 0; // index into turns()
};

class ReplayStore {
public:
  ReplayStore() = default;
  ReplayStore(const ReplayStore&) = delete;
  ReplayStore& operator=(const ReplayStore&) = delete;

  // Load <runDir>/events.jsonl + llm/* blobs. False with err when the run
  // is unusable for replay (missing files, malformed envelope/idx, torn
  // blobs). Zero recorded pairs is NOT an error here (the caller maps it
  // to exit 69 "replay impossible"); check empty() after load().
  bool load(const std::string& runDir, std::string& err);
  bool empty() const {
    return turns_.empty();
  }
  size_t size() const {
    return turns_.size();
  }
  const std::string& runDir() const {
    return runDir_;
  }

  // Match one incoming request; consumes the served turn. Thread-safe.
  // On MISS the request is appended to replay.unrecorded.jsonl (lazily
  // created) and {hit:false} is returned — the caller answers 502.
  ReplayMatch match(const std::string& provider, const std::string& method, const std::string& path,
                    const std::string& body);

  const RecordedTurn& turn(size_t slot) const {
    return turns_[slot];
  }
  long served() const {
    return served_.load();
  }
  long unrecorded() const {
    return unrecorded_.load();
  }

private:
  std::string runDir_;
  std::string unrecordedPath_;
  std::vector<RecordedTurn> turns_;
  mutable std::mutex mu_;
  std::atomic<long> served_{0};
  std::atomic<long> unrecorded_{0};
};

} // namespace snowglobe::replay

#pragma once
// Thread ownership: construct/configure on one thread, then use from a single
// writer thread. Not internally synchronised (tracer event loop is single-threaded).
#include <cstdint>
#include <fstream>
#include <string>

namespace snowglobe::trace {

// Buffered, line-flushed JSONL writer with the schema-v0 SHA-256 hash chain:
//   hash = sha256(prev_hash || canonical_json(event minus prev_hash/hash))
// `writeEvent` expects a JSON object string *without* seq/prev_hash/hash and
// returns the full line written (with seq, ts fields supplied by caller inside
// the payload, plus chain fields appended).
class JsonlWriter {
public:
  explicit JsonlWriter(const std::string& path);
  ~JsonlWriter();

  JsonlWriter(const JsonlWriter&) = delete;
  JsonlWriter& operator=(const JsonlWriter&) = delete;

  // Appends chain fields and writes one line. Flushes per event (crash-safe).
  // Returns false + sets error() on I/O failure.
  bool writeEvent(uint64_t seq, const std::string& jsonWithoutChain);
  bool ok() const {
    return ok_;
  }
  const std::string& error() const {
    return error_;
  }
  const std::string& lastHash() const {
    return lastHash_;
  }
  uint64_t count() const {
    return count_;
  }

private:
  std::ofstream out_;
  std::string lastHash_;
  uint64_t count_ = 0;
  bool ok_ = true;
  std::string error_;
};

} // namespace snowglobe::trace

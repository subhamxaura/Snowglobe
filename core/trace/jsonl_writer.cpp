#include "jsonl_writer.hpp"

#include "../util/sha256.hpp"

namespace snowglobe::trace {

JsonlWriter::JsonlWriter(const std::string& path) : out_(path, std::ios::out | std::ios::trunc), lastHash_("0") {
  if (!out_) {
    ok_ = false;
    error_ = "cannot open " + path;
  }
}

JsonlWriter::~JsonlWriter() {
  if (out_.is_open()) {
    out_.flush();
  }
}

bool JsonlWriter::writeEvent(uint64_t seq, const std::string& jsonWithoutChain) {
  if (!ok_) {
    return false;
  }
  // jsonWithoutChain must be a JSON object; inject chain fields before final '}'.
  // Find last '}' (payloads we generate always end with '}').
  const auto pos = jsonWithoutChain.find_last_of('}');
  if (pos == std::string::npos) {
    ok_ = false;
    error_ = "event payload is not a JSON object";
    return false;
  }
  std::string withSeq = jsonWithoutChain;
  std::string chainFields = "\"seq\":" + std::to_string(seq) + ",\"prev_hash\":\"" + lastHash_ + "\"";
  withSeq.insert(pos, (pos > 1 ? "," : "") + chainFields);

  const std::string hash = util::sha256Hex(lastHash_ + withSeq);
  std::string line = withSeq;
  // Insert before the final '}' (no extra brace: withSeq already ends with one).
  line.insert(line.find_last_of('}'), ",\"hash\":\"" + hash + "\"");

  out_ << line << "\n";
  out_.flush();
  if (!out_) {
    ok_ = false;
    error_ = "write failed";
    return false;
  }
  lastHash_ = hash;
  ++count_;
  return true;
}

}  // namespace snowglobe::trace

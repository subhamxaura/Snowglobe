#pragma once
// Replay request matching primitives (ADR-0010): minimal JSON parse,
// canonical serialization, volatile scrubbing, request hashing.
//
// Thread ownership: stateless free functions + const methods; safe from
// any thread. No <regex> (see test/tsan.supp for why regex is unwelcome
// here) — all scrubbing is hand-rolled scanners.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace snowglobe::replay {

struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  std::string number; // raw lexeme, verbatim
  std::string str;    // decoded (unescaped) string content
  std::vector<JsonValue> items;
  std::map<std::string, JsonValue> fields; // sorted by construction
};

// Strict JSON parse with a nesting cap (hostile bodies fail loud, never
// crash). Returns false with err set on any malformed input.
bool parseJson(const std::string& s, JsonValue& out, std::string& err);

// Canonical form: sorted object keys, minimal separators, strings
// re-escaped. Same logical value -> byte-identical output.
std::string canonicalJson(const JsonValue& v);

// Scrub volatile string patterns (ports of test/normalize.py rules):
// /tmp/tmp.<alnum+underscore>+ -> $TMP, 127.0.0.1:N -> 127.0.0.1:PORT,
// [::1]:N -> [::1]:PORT, python3.V / cpython-3V version folds.
std::string scrubString(const std::string& s);

// Normalize a request body for matching: JSON objects canonicalize with
// volatile keys dropped recursively (id/tool_call_id/tool_use_id/
// created/timestamp/ts/user/request_id/session_id) and strings scrubbed;
// non-JSON bodies hash raw ("raw:<sha256hex>").
std::string normalizeBody(const std::string& body);

// Canonical request hash: sha256hex(provider + "\n" + path + "\n" +
// normalizeBody(body)). Provider/path are matched verbatim (an endpoint
// change is a different call, never normalized away).
std::string requestHash(const std::string& provider, const std::string& path,
                        const std::string& body);

} // namespace snowglobe::replay

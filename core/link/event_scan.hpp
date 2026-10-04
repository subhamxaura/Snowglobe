#pragma once
// Top-level JSON object field scanner for trace events: extracts string,
// integer, boolean and string-array fields at object depth 1, without a
// JSON library. Same no-dependency style as proxy/model_scan.hpp (whose
// string/whitespace/value primitives are reused here).
//
// Numbers arrive as int64 (seq, pid, status, bytes); pid/tid/ppid may also
// be normalised strings ("P1") — callers take the verbatim raw form.
#include <cstdint>
#include <string>
#include <vector>

#include "../proxy/model_scan.hpp"

namespace snowglobe::link {

// A raw field value: type tag + verbatim text (strings unescaped).
struct RawField {
  bool found = false;
  bool isString = false;
  bool isNumber = false;
  bool isBool = false;
  bool isArray = false;
  std::string str; // unescaped when isString
  int64_t num = 0; // when isNumber (clamped on overflow)
  bool boolean = false;
};

// Locate top-level key `key` in the object starting at body[obj] ('{').
// On success sets val to the first index of the value and returns true.
inline bool findField(const std::string& body, size_t obj, const std::string& key, size_t& val) {
  using snowglobe::proxy::parseJsonString;
  using snowglobe::proxy::skipValue;
  using snowglobe::proxy::skipWs;
  const size_t n = body.size();
  if (obj >= n || body[obj] != '{') {
    return false;
  }
  size_t i = obj;
  int depth = 0;
  while (i < n) {
    const char c = body[i];
    if (c == '{') {
      ++depth;
      ++i;
      continue;
    }
    if (c == '}') {
      --depth;
      ++i;
      if (depth == 0) {
        return false;
      }
      continue;
    }
    if (c == '"' && depth == 1) {
      std::string k;
      const size_t e = parseJsonString(body, i, k);
      if (e == std::string::npos) {
        return false;
      }
      i = e;
      skipWs(body, i);
      if (i < n && body[i] == ':') {
        ++i;
        skipWs(body, i);
        if (k == key) {
          val = i;
          return true;
        }
        skipValue(body, i);
        continue;
      }
      continue;
    }
    ++i;
  }
  return false;
}

// Read the field at value index v (as returned by findField).
inline RawField readField(const std::string& body, size_t v) {
  using snowglobe::proxy::parseJsonString;
  using snowglobe::proxy::skipValue;
  RawField f;
  const size_t n = body.size();
  if (v >= n) {
    return f;
  }
  const char c = body[v];
  if (c == '"') {
    f.found = true;
    f.isString = true;
    const size_t e = parseJsonString(body, v, f.str);
    if (e == std::string::npos) {
      f.found = false;
      f.isString = false;
    }
    return f;
  }
  if (c == '[') {
    f.found = true;
    f.isArray = true;
    return f;
  }
  if (body.compare(v, 4, "true") == 0) {
    f.found = true;
    f.isBool = true;
    f.boolean = true;
    return f;
  }
  if (body.compare(v, 5, "false") == 0) {
    f.found = true;
    f.isBool = true;
    f.boolean = false;
    return f;
  }
  if (body.compare(v, 4, "null") == 0) {
    f.found = true; // present-but-null: found, no type flag
    return f;
  }
  if ((c >= '0' && c <= '9') || c == '-') {
    size_t e = v + (c == '-' ? 1 : 0);
    while (e < n && body[e] >= '0' && body[e] <= '9') {
      ++e;
    }
    if (e == v || (e == v + 1 && c == '-')) {
      return f;
    }
    f.found = true;
    f.isNumber = true;
    try {
      f.num = std::stoll(body.substr(v, e - v));
    } catch (...) {
      f.num = (c == '-') ? INT64_MIN : INT64_MAX; // clamp, never throw
    }
    return f;
  }
  return f;
}

// String-array field (e.g. argv): elements must ALL be strings;
// any non-string element (or unterminated array) fails the whole field.
inline bool getStrArray(const std::string& body, size_t arr, std::vector<std::string>& out) {
  using snowglobe::proxy::parseJsonString;
  using snowglobe::proxy::skipWs;
  out.clear();
  const size_t n = body.size();
  if (arr >= n || body[arr] != '[') {
    return false;
  }
  size_t i = arr + 1;
  skipWs(body, i);
  if (i < n && body[i] == ']') {
    return true; // empty array
  }
  while (i < n) {
    skipWs(body, i);
    if (i >= n || body[i] != '"') {
      return false;
    }
    std::string s;
    const size_t e = parseJsonString(body, i, s);
    if (e == std::string::npos) {
      return false;
    }
    out.push_back(s);
    i = e;
    skipWs(body, i);
    if (i < n && body[i] == ',') {
      ++i;
      continue;
    }
    if (i < n && body[i] == ']') {
      return true;
    }
    return false;
  }
  return false;
}

} // namespace snowglobe::link

#pragma once
// Top-level JSON field scanner: extracts "model" (string) and "stream"
// (bool) at object depth 1, without a JSON library. Provider parsing lives
// in the TypeScript viewer; C++ sees only these two routing/display fields
// (AGENTS.md §1.11, Block 1 item 4). Nested "model" keys (e.g. a function
// parameter named "model") must NOT match — unit-tested.
#include <string>

namespace snowglobe::proxy {

struct JsonTop {
  bool isJson = false;
  bool hasModel = false;
  std::string model;
  bool hasStream = false;
  bool stream = false;
};

// Parse a JSON string starting at body[i] (body[i] == '"').
// Returns end index (one past closing quote), or npos on unterminated.
inline size_t parseJsonString(const std::string& body, size_t i, std::string& out) {
  out.clear();
  const size_t n = body.size();
  ++i; // opening quote
  while (i < n) {
    const char c = body[i];
    if (c == '\\' && i + 1 < n) {
      out.push_back(body[i + 1]); // unescape flatly (good enough for model names)
      i += 2;
      continue;
    }
    if (c == '"') {
      return i + 1;
    }
    out.push_back(c);
    ++i;
  }
  return std::string::npos;
}

inline void skipWs(const std::string& body, size_t& i) {
  while (i < body.size() &&
         (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) {
    ++i;
  }
}

// Skip one JSON value starting at i (string/object/array/literal).
inline void skipValue(const std::string& body, size_t& i) {
  const size_t n = body.size();
  if (i >= n) {
    return;
  }
  if (body[i] == '"') {
    std::string tmp;
    const size_t e = parseJsonString(body, i, tmp);
    i = (e == std::string::npos) ? n : e;
    return;
  }
  if (body[i] == '{' || body[i] == '[') {
    int depth = 0;
    bool inS = false;
    while (i < n) {
      const char c = body[i];
      if (inS) {
        if (c == '\\') {
          i += 2;
          continue;
        }
        if (c == '"') {
          inS = false;
        }
        ++i;
        continue;
      }
      if (c == '"') {
        inS = true;
      } else if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        --depth;
        if (depth == 0) {
          ++i;
          return;
        }
      }
      ++i;
    }
    return;
  }
  while (i < n && body[i] != ',' && body[i] != '}' && body[i] != ']' && body[i] != ' ' &&
         body[i] != '\t' && body[i] != '\n' && body[i] != '\r') {
    ++i;
  }
}

inline JsonTop scanJsonTop(const std::string& body) {
  JsonTop r;
  size_t i = 0;
  skipWs(body, i);
  const size_t n = body.size();
  if (i >= n || body[i] != '{') {
    return r; // not a JSON object (arrays/scalars/plain text -> null fields)
  }
  r.isJson = true;
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
        return r;
      }
      continue;
    }
    if (c == '"' && depth == 1) {
      std::string key;
      const size_t e = parseJsonString(body, i, key);
      if (e == std::string::npos) {
        return r;
      }
      i = e;
      skipWs(body, i);
      if (i < n && body[i] == ':') {
        ++i;
        skipWs(body, i);
        if (key == "model" && i < n && body[i] == '"') {
          std::string val;
          const size_t ve = parseJsonString(body, i, val);
          if (ve != std::string::npos) {
            r.hasModel = true;
            r.model = val;
            i = ve;
            if (r.hasStream) {
              return r;
            }
            continue;
          }
        } else if (key == "stream" && i < n) {
          if (body.compare(i, 4, "true") == 0) {
            r.hasStream = true;
            r.stream = true;
            i += 4;
            if (r.hasModel) {
              return r;
            }
            continue;
          }
          if (body.compare(i, 5, "false") == 0) {
            r.hasStream = true;
            r.stream = false;
            i += 5;
            if (r.hasModel) {
              return r;
            }
            continue;
          }
        }
        skipValue(body, i); // nested/complex/unwanted value
        continue;
      }
      continue;
    }
    ++i;
  }
  return r;
}

} // namespace snowglobe::proxy

#pragma once
// Tool-call extraction from LLM response bodies (C++ side sees only the
// command text it needs for argv matching; all provider parsing lives in
// the TypeScript viewer per AGENTS.md §1.11).
//
// Shapes (both non-streamed JSON):
//   OpenAI:  choices[0].message.tool_calls[] = {id, function:{name,
//              arguments}} where arguments is a JSON-encoded string (or,
//              defensively, an object) carrying a "command" string.
//   Anthropic: content[] blocks with type=="tool_use" = {id, name, input}
//              where input is an object carrying a "command" string.
// Only the depth-1 "command" key counts (a nested parameter literally
// named "command" must not match — unit-tested). SSE / binary /
// non-JSON bodies yield no tools (never throw on agent traffic).
#include <cstddef>
#include <string>
#include <vector>

#include "event_scan.hpp"

namespace snowglobe::link {

struct ToolCall {
  std::string id;
  std::string name;
  std::string command; // empty when the call carries no command text
};

// Start index of the idx-th element of the array at body[arr] ('['),
// or npos when missing/short/unterminated.
inline size_t arrayElem(const std::string& body, size_t arr, size_t idx) {
  using snowglobe::proxy::parseJsonString;
  using snowglobe::proxy::skipValue;
  using snowglobe::proxy::skipWs;
  const size_t n = body.size();
  if (arr >= n || body[arr] != '[') {
    return std::string::npos;
  }
  size_t i = arr + 1;
  for (size_t k = 0;; ++k) {
    skipWs(body, i);
    if (i >= n) {
      return std::string::npos;
    }
    if (body[i] == ']') {
      return std::string::npos; // short array (k == size: ']' ends it)
    }
    if (k == idx) {
      return i;
    }
    skipValue(body, i);
    skipWs(body, i);
    if (i < n && body[i] == ',') {
      ++i;
      continue;
    }
    return std::string::npos;
  }
}

// Depth-1 string member of the object at body[obj] ('{').
inline bool objectStr(const std::string& body, size_t obj, const std::string& key,
                      std::string& out) {
  size_t v = 0;
  if (!findField(body, obj, key, v)) {
    return false;
  }
  const RawField f = readField(body, v);
  if (!f.found || !f.isString) {
    return false;
  }
  out = f.str;
  return true;
}

// Depth-1 member that must be an object ('{'); sets obj to its start.
inline bool objectObj(const std::string& body, size_t obj, const std::string& key, size_t& sub) {
  size_t v = 0;
  if (!findField(body, obj, key, v)) {
    return false;
  }
  if (v >= body.size() || body[v] != '{') {
    return false;
  }
  sub = v;
  return true;
}

// The "command" string inside an arguments/input object (depth 1 only).
inline bool commandOf(const std::string& body, size_t obj, std::string& out) {
  return objectStr(body, obj, "command", out);
}

// arguments may be a JSON-encoded string (OpenAI) or an inline object.
inline bool argumentsCommand(const std::string& body, size_t v, std::string& out) {
  if (v >= body.size()) {
    return false;
  }
  if (body[v] == '"') {
    std::string inner;
    std::string tmp;
    const size_t e = snowglobe::proxy::parseJsonString(body, v, tmp);
    if (e == std::string::npos) {
      return false;
    }
    inner = tmp;
    size_t ws = 0;
    snowglobe::proxy::skipWs(inner, ws);
    if (ws >= inner.size() || inner[ws] != '{') {
      return false;
    }
    return commandOf(inner, ws, out);
  }
  if (body[v] == '{') {
    return commandOf(body, v, out);
  }
  return false;
}

inline std::vector<ToolCall> extractOpenAi(const std::string& body) {
  std::vector<ToolCall> out;
  size_t v = 0;
  if (!findField(body, 0, "choices", v) || v >= body.size() || body[v] != '[') {
    return out;
  }
  const size_t c0 = arrayElem(body, v, 0);
  if (c0 == std::string::npos || c0 >= body.size() || body[c0] != '{') {
    return out;
  }
  size_t msg = 0;
  if (!objectObj(body, c0, "message", msg)) {
    return out;
  }
  if (!findField(body, msg, "tool_calls", v) || v >= body.size() || body[v] != '[') {
    return out;
  }
  for (size_t k = 0;; ++k) {
    const size_t e = arrayElem(body, v, k);
    if (e == std::string::npos || e >= body.size() || body[e] != '{') {
      break;
    }
    ToolCall t;
    objectStr(body, e, "id", t.id);
    size_t fn = 0;
    if (objectObj(body, e, "function", fn)) {
      objectStr(body, fn, "name", t.name);
      size_t av = 0;
      if (findField(body, fn, "arguments", av)) {
        std::string cmd;
        if (argumentsCommand(body, av, cmd)) {
          t.command = cmd;
        }
      }
    }
    if (!t.id.empty() || !t.name.empty()) {
      out.push_back(t);
    }
  }
  return out;
}

inline std::vector<ToolCall> extractAnthropic(const std::string& body) {
  std::vector<ToolCall> out;
  size_t v = 0;
  if (!findField(body, 0, "content", v) || v >= body.size() || body[v] != '[') {
    return out;
  }
  for (size_t k = 0;; ++k) {
    const size_t e = arrayElem(body, v, k);
    if (e == std::string::npos || e >= body.size() || body[e] != '{') {
      break;
    }
    std::string type;
    if (!objectStr(body, e, "type", type) || type != "tool_use") {
      continue;
    }
    ToolCall t;
    objectStr(body, e, "id", t.id);
    objectStr(body, e, "name", t.name);
    size_t input = 0;
    if (objectObj(body, e, "input", input)) {
      std::string cmd;
      if (commandOf(body, input, cmd)) {
        t.command = cmd;
      }
    }
    if (!t.id.empty() || !t.name.empty()) {
      out.push_back(t);
    }
  }
  return out;
}

// provider: "openai" | "anthropic" | other (other tries both shapes).
inline std::vector<ToolCall> extractTools(const std::string& body, const std::string& provider) {
  std::string s = body;
  size_t i = 0;
  snowglobe::proxy::skipWs(s, i);
  if (i >= s.size() || s[i] != '{') {
    return {}; // SSE, binary, text and truncated prefixes: no tools, no throw
  }
  if (provider == "anthropic") {
    return extractAnthropic(s);
  }
  if (provider == "openai") {
    return extractOpenAi(s);
  }
  std::vector<ToolCall> out = extractOpenAi(s);
  const std::vector<ToolCall> alt = extractAnthropic(s);
  out.insert(out.end(), alt.begin(), alt.end());
  return out;
}

} // namespace snowglobe::link

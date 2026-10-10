// Replay request matching primitives (ADR-0010).
#include "replay_match.hpp"

#include <cctype>
#include <cstdio>

#include "../util/sha256.hpp"

namespace snowglobe::replay {
namespace {

constexpr int kMaxDepth = 100;

bool isVolatileKey(const std::string& k) {
  return k == "id" || k == "tool_call_id" || k == "tool_use_id" || k == "created" ||
         k == "timestamp" || k == "ts" || k == "user" || k == "request_id" || k == "session_id";
}

struct Parser {
  const char* p = nullptr;
  size_t n = 0;
  size_t i = 0;
  std::string err;

  bool fail(const std::string& msg) {
    err = msg + " at offset " + std::to_string(i);
    return false;
  }

  void ws() {
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) {
      ++i;
    }
  }

  bool parseString(std::string& out) {
    // p[i] == '"'.
    out.clear();
    ++i;
    while (i < n) {
      const char c = p[i];
      if (c == '"') {
        ++i;
        return true;
      }
      if (c == '\\') {
        ++i;
        if (i >= n) {
          break;
        }
        const char e = p[i];
        switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          // \uXXXX: fold BMP escapes flatly. Model names / ids are ASCII;
          // a lossy-but-deterministic fold beats dragging UTF-16
          // surrogate logic into the matcher (matching only needs
          // stability, and both sides fold identically).
          if (i + 4 >= n) {
            return fail("bad \\u escape");
          }
          unsigned cp = 0;
          for (int k = 1; k <= 4; ++k) {
            const char h = p[i + k];
            cp <<= 4;
            if (h >= '0' && h <= '9') {
              cp += static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              cp += static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              cp += static_cast<unsigned>(h - 'A' + 10);
            } else {
              return fail("bad \\u escape");
            }
          }
          if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
          } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
          i += 5;
          continue;
        }
        default: return fail("bad escape");
        }
        ++i;
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return fail("unescaped control");
      }
      out.push_back(c);
      ++i;
    }
    return fail("unterminated string");
  }

  bool parseValue(JsonValue& out, int depth) {
    if (depth > kMaxDepth) {
      return fail("nesting too deep");
    }
    ws();
    if (i >= n) {
      return fail("unexpected end");
    }
    const char c = p[i];
    if (c == '{') {
      out.type = JsonValue::Type::Object;
      ++i;
      ws();
      if (i < n && p[i] == '}') {
        ++i;
        return true;
      }
      while (true) {
        ws();
        if (i >= n || p[i] != '"') {
          return fail("want object key");
        }
        std::string key;
        if (!parseString(key)) {
          return false;
        }
        ws();
        if (i >= n || p[i] != ':') {
          return fail("want ':'");
        }
        ++i;
        JsonValue val;
        if (!parseValue(val, depth + 1)) {
          return false;
        }
        out.fields[key] = std::move(val);
        ws();
        if (i >= n) {
          return fail("unterminated object");
        }
        if (p[i] == ',') {
          ++i;
          continue;
        }
        if (p[i] == '}') {
          ++i;
          return true;
        }
        return fail("want ',' or '}'");
      }
    }
    if (c == '[') {
      out.type = JsonValue::Type::Array;
      ++i;
      ws();
      if (i < n && p[i] == ']') {
        ++i;
        return true;
      }
      while (true) {
        JsonValue val;
        if (!parseValue(val, depth + 1)) {
          return false;
        }
        out.items.push_back(std::move(val));
        ws();
        if (i >= n) {
          return fail("unterminated array");
        }
        if (p[i] == ',') {
          ++i;
          continue;
        }
        if (p[i] == ']') {
          ++i;
          return true;
        }
        return fail("want ',' or ']'");
      }
    }
    if (c == '"') {
      out.type = JsonValue::Type::String;
      return parseString(out.str);
    }
    if (c == 't' && n - i >= 4 && p[i + 1] == 'r' && p[i + 2] == 'u' && p[i + 3] == 'e') {
      out.type = JsonValue::Type::Bool;
      out.boolean = true;
      i += 4;
      return true;
    }
    if (c == 'f' && n - i >= 5 && p[i + 1] == 'a' && p[i + 2] == 'l' && p[i + 3] == 's' &&
        p[i + 4] == 'e') {
      out.type = JsonValue::Type::Bool;
      out.boolean = false;
      i += 5;
      return true;
    }
    if (c == 'n' && n - i >= 4 && p[i + 1] == 'u' && p[i + 2] == 'l' && p[i + 3] == 'l') {
      out.type = JsonValue::Type::Null;
      i += 4;
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const size_t start = i;
      if (c == '-') {
        ++i;
      }
      if (i >= n) {
        return fail("bad number");
      }
      if (p[i] == '0') {
        ++i;
      } else if (p[i] >= '1' && p[i] <= '9') {
        while (i < n && p[i] >= '0' && p[i] <= '9') {
          ++i;
        }
      } else {
        return fail("bad number");
      }
      if (i < n && p[i] == '.') {
        ++i;
        if (i >= n || p[i] < '0' || p[i] > '9') {
          return fail("bad number");
        }
        while (i < n && p[i] >= '0' && p[i] <= '9') {
          ++i;
        }
      }
      if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        ++i;
        if (i < n && (p[i] == '+' || p[i] == '-')) {
          ++i;
        }
        if (i >= n || p[i] < '0' || p[i] > '9') {
          return fail("bad number");
        }
        while (i < n && p[i] >= '0' && p[i] <= '9') {
          ++i;
        }
      }
      out.type = JsonValue::Type::Number;
      out.number.assign(p + start, i - start);
      return true;
    }
    return fail("unexpected character");
  }
};

void appendEscaped(const std::string& s, std::string& out) {
  out.push_back('"');
  for (const unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20) {
        char buf[8] = {};
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  out.push_back('"');
}

void appendCanonical(const JsonValue& v, std::string& out) {
  switch (v.type) {
  case JsonValue::Type::Null: out += "null"; break;
  case JsonValue::Type::Bool: out += v.boolean ? "true" : "false"; break;
  case JsonValue::Type::Number: out += v.number; break;
  case JsonValue::Type::String: appendEscaped(v.str, out); break;
  case JsonValue::Type::Array:
    out.push_back('[');
    for (size_t k = 0; k < v.items.size(); ++k) {
      if (k > 0) {
        out.push_back(',');
      }
      appendCanonical(v.items[k], out);
    }
    out.push_back(']');
    break;
  case JsonValue::Type::Object:
    out.push_back('{');
    {
      bool first = true;
      for (const auto& kv : v.fields) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        appendEscaped(kv.first, out);
        out.push_back(':');
        appendCanonical(kv.second, out);
      }
    }
    out.push_back('}');
    break;
  }
}

// Hand-rolled scanners (no <regex>): single pass each, in normalize.py
// order ($TMP, python versions, ports).
void scrubTmp(std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t k = 0; k < s.size();) {
    if (s.compare(k, 9, "/tmp/tmp.") == 0) {
      size_t e = k + 9;
      // mkdtemp alphabet is [a-z0-9_] (underscore included — stranding
      // "_CD" as residue breaks cross-run equality; see normalize.py).
      while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) != 0 || s[e] == '_')) {
        ++e;
      }
      if (e > k + 9) {
        out += "$TMP";
        k = e;
        continue;
      }
    }
    out.push_back(s[k]);
    ++k;
  }
  s.swap(out);
}

void scrubPyVer(std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t k = 0; k < s.size();) {
    if (s.compare(k, 8, "python3.") == 0 && k + 8 < s.size() &&
        std::isdigit(static_cast<unsigned char>(s[k + 8])) != 0) {
      size_t e = k + 8;
      while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])) != 0) {
        ++e;
      }
      out += "python3.V";
      k = e;
      continue;
    }
    if (s.compare(k, 9, "cpython-3") == 0) {
      size_t e = k + 9;
      while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])) != 0) {
        ++e;
      }
      if (e > k + 9) {
        out += "cpython-3V";
        k = e;
        continue;
      }
    }
    out.push_back(s[k]);
    ++k;
  }
  s.swap(out);
}

void scrubPorts(std::string& s) {
  std::string out;
  out.reserve(s.size());
  static const char* kV4 = "127.0.0.1:";
  static const char* kV6 = "[::1]:";
  for (size_t k = 0; k < s.size();) {
    const char* pre = nullptr;
    size_t preLen = 0;
    if (s.compare(k, 10, kV4) == 0) {
      pre = kV4;
      preLen = 10;
    } else if (s.compare(k, 6, kV6) == 0) {
      pre = kV6;
      preLen = 6;
    }
    if (pre != nullptr) {
      size_t e = k + preLen;
      while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])) != 0) {
        ++e;
      }
      if (e > k + preLen) {
        out += pre;
        out += "PORT";
        k = e;
        continue;
      }
    }
    out.push_back(s[k]);
    ++k;
  }
  s.swap(out);
}

void normalizeValue(JsonValue& v) {
  switch (v.type) {
  case JsonValue::Type::String: v.str = scrubString(v.str); break;
  case JsonValue::Type::Array:
    for (auto& it : v.items) {
      normalizeValue(it);
    }
    break;
  case JsonValue::Type::Object: {
    // Erase-then-recurse (never mutate the key set while iterating it).
    for (auto it = v.fields.begin(); it != v.fields.end();) {
      if (isVolatileKey(it->first)) {
        it = v.fields.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& kv : v.fields) {
      normalizeValue(kv.second);
    }
    break;
  }
  case JsonValue::Type::Null:
  case JsonValue::Type::Bool:
  case JsonValue::Type::Number: break;
  }
}

} // namespace

bool parseJson(const std::string& s, JsonValue& out, std::string& err) {
  Parser ps;
  ps.p = s.data();
  ps.n = s.size();
  ps.i = 0;
  out = JsonValue();
  if (!ps.parseValue(out, 0)) {
    err = ps.err;
    return false;
  }
  ps.ws();
  if (ps.i != ps.n) {
    err = "trailing bytes at offset " + std::to_string(ps.i);
    return false;
  }
  return true;
}

std::string canonicalJson(const JsonValue& v) {
  std::string out;
  appendCanonical(v, out);
  return out;
}

std::string scrubString(const std::string& s) {
  std::string r = s;
  scrubTmp(r);
  scrubPyVer(r);
  scrubPorts(r);
  return r;
}

std::string normalizeBody(const std::string& body) {
  JsonValue v;
  std::string err;
  if (!parseJson(body, v, err)) {
    return "raw:" + util::sha256Hex(body);
  }
  normalizeValue(v);
  return canonicalJson(v);
}

std::string requestHash(const std::string& provider, const std::string& path,
                        const std::string& body) {
  return util::sha256Hex(provider + "\n" + path + "\n" + normalizeBody(body));
}

} // namespace snowglobe::replay

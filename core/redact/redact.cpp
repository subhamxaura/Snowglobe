// Shared secrets redaction. See ADR-0003 for the policy and its limits.
#include "redact.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

namespace snowglobe::redact {
namespace {

bool isTokenChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_' || c == '~' || c == '+' || c == '/' || c == '=';
}

bool isKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_';
}

bool isAwsKeyChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
}

// Consume [class]+ from pos; returns end index.
template <typename Pred> size_t takeWhile(const std::string& s, size_t pos, Pred pred) {
  while (pos < s.size() && pred(s[pos])) {
    ++pos;
  }
  return pos;
}

bool startsWith(const std::string& s, size_t pos, const char* pre) {
  const size_t L = std::strlen(pre);
  return s.compare(pos, L, pre, L) == 0;
}

bool startsWithI(const std::string& s, size_t pos, const char* pre) {
  const size_t L = std::strlen(pre);
  if (pos + L > s.size()) {
    return false;
  }
  for (size_t i = 0; i < L; ++i) {
    if (std::tolower((unsigned char)s[pos + i]) != std::tolower((unsigned char)pre[i])) {
      return false;
    }
  }
  return true;
}

bool envNameSensitive(const std::string& name) {
  // Suffix set is exactly AGENTS.md §1.9 / ADR-0003: KEY|TOKEN|SECRET|
  // PASSWORD|PASSWD|CREDENTIAL (case-insensitive). Notably NOT bare "pass"
  // (would match COMPASS/BYPASS); extend only via ADR.
  static constexpr std::string_view kSuffix[] = {"key",      "token",  "secret",
                                                 "password", "passwd", "credential"};
  std::string n = name;
  for (char& c : n) {
    c = static_cast<char>(std::tolower((unsigned char)c));
  }
  for (std::string_view suf : kSuffix) {
    if (n.size() >= suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0) {
      return true;
    }
  }
  return false;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) {
    return;
  }
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

} // namespace

bool isRedactedHeader(const std::string& name) {
  std::string n = name;
  for (char& c : n) {
    c = static_cast<char>(std::tolower((unsigned char)c));
  }
  return n == "authorization" || n == "x-api-key" || n == "api-key" || n == "cookie" ||
         n == "set-cookie" || n == "proxy-authorization" || n == "x-goog-api-key";
}

std::string redactHeaderValue(const std::string& name, const std::string& value) {
  if (isRedactedHeader(name)) {
    return kRedacted;
  }
  return value;
}

std::vector<std::pair<std::string, std::string>> sensitiveEnv(char* const* envp) {
  std::vector<std::pair<std::string, std::string>> out;
  if (envp == nullptr) {
    return out;
  }
  for (char* const* e = envp; *e != nullptr; ++e) {
    const std::string entry(*e);
    const size_t eq = entry.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string name = entry.substr(0, eq);
    const std::string value = entry.substr(eq + 1);
    if (value.size() >= 8 && envNameSensitive(name)) {
      out.emplace_back(name, value);
    }
  }
  return out;
}

std::string redactText(const std::string& text,
                       const std::vector<std::pair<std::string, std::string>>& env) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  const size_t n = text.size();

  while (i < n) {
    // URL userinfo: scheme://user:pass@host — keep user, redact password.
    // (Requires "://", so bare user@host emails never match.)
    if (text[i] == ':' && startsWith(text, i + 1, "//")) {
      size_t at = i + 3;
      while (at < n && text[at] != '@' && text[at] != '/' && text[at] != ' ' && text[at] != '"' &&
             text[at] != '\'') {
        ++at;
      }
      if (at < n && text[at] == '@') {
        const std::string userinfo = text.substr(i + 3, at - (i + 3));
        const size_t colon = userinfo.find(':');
        out += "://";
        if (colon != std::string::npos) {
          out += userinfo.substr(0, colon);
          out += ":";
          out += kRedacted;
        } else {
          out += kRedacted;
        }
        out += "@";
        i = at + 1;
        continue;
      }
    }
    // "Bearer <token>" / "Basic <credentials>" (case-insensitive scheme,
    // token min 8 chars, else likely prose).
    const char* schemeOut = nullptr;
    size_t schemeLen = 0;
    if (startsWithI(text, i, "bearer")) {
      schemeOut = "Bearer ";
      schemeLen = 6;
    } else if (startsWithI(text, i, "basic")) {
      schemeOut = "Basic ";
      schemeLen = 5;
    }
    if (schemeOut != nullptr && (i == 0 || !isTokenChar(text[i - 1]))) {
      size_t j = i + schemeLen;
      while (j < n && (text[j] == ' ' || text[j] == '\t')) {
        ++j;
      }
      const size_t end = takeWhile(text, j, isTokenChar);
      if (end - j >= 8) {
        out += schemeOut;
        out += kRedacted;
        i = end;
        continue;
      }
    }
    if (text[i] == 's' && startsWith(text, i, "sk-ant-")) {
      const size_t end = takeWhile(text, i + 7, isKeyChar);
      if (end > i + 7) {
        out += kRedacted;
        i = end;
        continue;
      }
    }
    if (text[i] == 's' && startsWith(text, i, "sk-")) {
      const size_t end = takeWhile(text, i + 3, isKeyChar);
      if (end - (i + 3) >= 20) {
        out += kRedacted;
        i = end;
        continue;
      }
    }
    if (text[i] == 'g') {
      const size_t pre = startsWith(text, i, "github_pat_") ? 11 : 4;
      if (startsWith(text, i, "ghp_") || startsWith(text, i, "gho_") ||
          startsWith(text, i, "github_pat_")) {
        const size_t end = takeWhile(text, i + pre, isKeyChar);
        if (end - (i + pre) >= 20) {
          out += kRedacted;
          i = end;
          continue;
        }
      }
    }
    if (startsWith(text, i, "AKIA")) {
      size_t j = i + 4;
      size_t digits = 0;
      while (j < n && digits < 16 && isAwsKeyChar(text[j])) {
        ++j;
        ++digits;
      }
      if (digits == 16 && (j >= n || !isKeyChar(text[j]))) {
        out += kRedacted;
        i = j;
        continue;
      }
    }
    if (text[i] == 'x' && (startsWith(text, i, "xoxa-") || startsWith(text, i, "xoxb-") ||
                           startsWith(text, i, "xoxp-"))) {
      const size_t end = takeWhile(text, i + 5, isKeyChar);
      if (end > i + 5) {
        out += kRedacted;
        i = end;
        continue;
      }
    }
    // ?key= / &token= / #password= (case-insensitive name, non-empty value).
    if (text[i] == '?' || text[i] == '&' || text[i] == '#') {
      static const char* kNames[] = {"key=", "token=", "password="};
      bool consumed = false;
      for (const char* nm : kNames) {
        const size_t L = std::strlen(nm);
        if (startsWithI(text, i + 1, nm)) {
          const size_t vs = i + 1 + L;
          size_t ve = vs;
          while (ve < n && text[ve] != '&' && text[ve] != '#' && text[ve] != ' ' &&
                 text[ve] != '"' && text[ve] != '\'') {
            ++ve;
          }
          if (ve > vs) {
            out.push_back(text[i]);
            out += text.substr(i + 1, L);
            out += kRedacted;
            i = ve;
            consumed = true;
          }
          break;
        }
      }
      if (consumed) {
        continue;
      }
    }
    out.push_back(text[i]);
    ++i;
  }

  // Environment values, longest first (a short value that prefixes a longer
  // one must not win).
  std::vector<std::pair<std::string, std::string>> sorted = env;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
  for (const auto& [name, value] : sorted) {
    (void)name;
    if (value.size() >= 8) {
      replaceAll(out, value, kRedacted);
    }
  }
  return out;
}

} // namespace snowglobe::redact

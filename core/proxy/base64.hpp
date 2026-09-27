#pragma once
// Base64 (standard) + base64url (no padding on decode input). Header-only.
// Used for /u/<base64url-upstream> route decoding and non-UTF8 body wrap.
#include <cstdint>
#include <string>

namespace snowglobe::proxy {

inline std::string base64Encode(const std::string& in) {
  static const char* kTab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
    size_t n = 1;
    if (i + 1 < in.size()) {
      v |= (uint32_t)(unsigned char)in[i + 1] << 8;
      ++n;
    }
    if (i + 2 < in.size()) {
      v |= (uint32_t)(unsigned char)in[i + 2];
      ++n;
    }
    out.push_back(kTab[(v >> 18) & 63]);
    out.push_back(kTab[(v >> 12) & 63]);
    out.push_back(n > 1 ? kTab[(v >> 6) & 63] : '=');
    out.push_back(n > 2 ? kTab[v & 63] : '=');
  }
  return out;
}

// Returns false on any character outside the alphabet (after url-fixups).
inline bool base64Decode(const std::string& in, std::string& out, bool url) {
  out.clear();
  std::string norm;
  norm.reserve(in.size() + 4);
  for (char c : in) {
    if (c == '-' && url) {
      norm.push_back('+');
    } else if (c == '_' && url) {
      norm.push_back('/');
    } else {
      norm.push_back(c);
    }
  }
  while (norm.size() % 4 != 0) {
    norm.push_back('=');
  }
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') {
      return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
      return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
      return c - '0' + 52;
    }
    if (c == '+') {
      return 62;
    }
    if (c == '/') {
      return 63;
    }
    return -1;
  };
  out.reserve((norm.size() / 4) * 3);
  for (size_t i = 0; i < norm.size(); i += 4) {
    int a = val(norm[i]), b = val(norm[i + 1]);
    int c = norm[i + 2] == '=' ? 0 : val(norm[i + 2]);
    int d = norm[i + 3] == '=' ? 0 : val(norm[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) {
      out.clear();
      return false;
    }
    const uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
    out.push_back((char)((v >> 16) & 255));
    if (norm[i + 2] != '=') {
      out.push_back((char)((v >> 8) & 255));
    }
    if (norm[i + 3] != '=') {
      out.push_back((char)(v & 255));
    }
  }
  return true;
}

inline bool isUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    size_t want = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      want = 1;
    } else if ((c & 0xF0) == 0xE0) {
      want = 2;
    } else if ((c & 0xF8) == 0xF0) {
      want = 3;
    } else {
      return false;
    }
    if (i + want >= s.size()) {
      return false;
    }
    for (size_t k = 1; k <= want; ++k) {
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) {
        return false;
      }
    }
    i += 1 + want;
  }
  return true;
}

} // namespace snowglobe::proxy

#include "string_util.hpp"

#include <cstdio>

#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace snowglobe::util {

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (unsigned char c : s) {
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
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  out.push_back('"');
  return out;
}

std::string formatSockaddr(const void* addr, unsigned long addrLen) {
  return parseSockaddr(addr, addrLen).display;
}

SockaddrParts parseSockaddr(const void* addr, unsigned long addrLen) {
#ifdef __linux__
  SockaddrParts out;
  if (addr == nullptr || addrLen < sizeof(sa_family_t)) {
    return out;
  }
  const auto* sa = static_cast<const struct sockaddr*>(addr);
  if (sa->sa_family == AF_INET && addrLen >= sizeof(struct sockaddr_in)) {
    const auto* in = static_cast<const struct sockaddr_in*>(addr);
    char ip[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip)) == nullptr) {
      return out;
    }
    out.family = "ipv4";
    out.ip = ip;
    out.port = static_cast<long>(ntohs(in->sin_port));
    out.display = out.ip + ":" + std::to_string(out.port);
    return out;
  }
  if (sa->sa_family == AF_INET6 && addrLen >= sizeof(struct sockaddr_in6)) {
    const auto* in6 = static_cast<const struct sockaddr_in6*>(addr);
    char ip[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip)) == nullptr) {
      return out;
    }
    out.family = "ipv6";
    out.ip = ip;
    out.port = static_cast<long>(ntohs(in6->sin6_port));
    out.display = std::string("[") + out.ip + "]:" + std::to_string(out.port);
    return out;
  }
  if (sa->sa_family == AF_UNIX) {
    const auto* un = static_cast<const struct sockaddr_un*>(addr);
    // addrLen includes sun_family; path may not be NUL-terminated (abstract).
    std::size_t pathLen = 0;
    if (addrLen > sizeof(sa_family_t)) {
      pathLen = static_cast<std::size_t>(addrLen) - sizeof(sa_family_t);
    }
    std::string path(un->sun_path, 0, pathLen);
    const auto nul = path.find('\0');
    if (nul != std::string::npos) {
      path.resize(nul);
    }
    out.family = "unix";
    out.path = path;
    out.display = "unix:" + path;
    return out;
  }
  if (sa->sa_family == AF_UNSPEC) {
    // UDP-disconnect idiom (resolver unconnect): no peer at all.
    out.family = "unspec";
    out.display = "family=0";
    return out;
  }
  out.family = "family=" + std::to_string(static_cast<int>(sa->sa_family));
  out.display = out.family;
  return out;
#else
  (void)addr;
  (void)addrLen;
  return SockaddrParts{};
#endif
}

} // namespace snowglobe::util

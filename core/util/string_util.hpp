#pragma once
// Thread ownership: stateless free functions, safe to call from any thread.
#include <string>

namespace snowglobe::util {

// Escape a string for inclusion in JSON (adds surrounding quotes).
std::string jsonEscape(const std::string& s);

// Format a raw sockaddr for net.connect events.
// Returns e.g. "127.0.0.1:8080", "[::1]:80", "unix:/tmp/s.sock".
std::string formatSockaddr(const void* addr, unsigned long addrLen);

// Structured decode of a raw sockaddr for net.* events. display is the
// legacy formatSockaddr string (kept as "addr" verbatim); family is one of
// ipv4|ipv6|unix|unspec|unknown (other families pass through as
// "family=N"); ip/port are set for ipv4/ipv6 (port -1 when n/a), path for
// unix. AF_UNSPEC (family 0, the UDP-disconnect idiom) decodes to unspec.
struct SockaddrParts {
  std::string family = "unknown";
  std::string display = "unknown";
  std::string ip;
  long port = -1;
  std::string path;
};
SockaddrParts parseSockaddr(const void* addr, unsigned long addrLen);

} // namespace snowglobe::util

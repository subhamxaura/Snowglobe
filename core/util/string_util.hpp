#pragma once
// Thread ownership: stateless free functions, safe to call from any thread.
#include <string>

namespace snowglobe::util {

// Escape a string for inclusion in JSON (adds surrounding quotes).
std::string jsonEscape(const std::string& s);

// Format a raw sockaddr for net.connect events.
// Returns e.g. "127.0.0.1:8080", "[::1]:80", "unix:/tmp/s.sock".
std::string formatSockaddr(const void* addr, unsigned long addrLen);

}  // namespace snowglobe::util

// Unit: sockaddr struct decode (parseSockaddr) + legacy display parity.
// Pure function tests, no kernel needed — run on every preset/CI leg.
#include "util/string_util.hpp"

#include <catch2/catch_test_macros.hpp>

#ifdef __linux__
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

using snowglobe::util::formatSockaddr;
using snowglobe::util::parseSockaddr;

TEST_CASE("null and short inputs stay unknown", "[sockaddr]") {
  const auto p = parseSockaddr(nullptr, 0);
  CHECK(p.family == "unknown");
  CHECK(p.display == "unknown");
  CHECK(formatSockaddr(nullptr, 0) == "unknown");
}

#ifdef __linux__
TEST_CASE("ipv4 splits family, ip, port", "[sockaddr]") {
  struct sockaddr_in in = {};
  in.sin_family = AF_INET;
  in.sin_port = htons(8080);
  in.sin_addr.s_addr = htonl(0x7F000001);
  const auto p = parseSockaddr(&in, sizeof(in));
  CHECK(p.family == "ipv4");
  CHECK(p.ip == "127.0.0.1");
  CHECK(p.port == 8080);
  CHECK(p.display == "127.0.0.1:8080");
  CHECK(formatSockaddr(&in, sizeof(in)) == "127.0.0.1:8080");
}

TEST_CASE("ipv6 splits family, ip, port", "[sockaddr]") {
  struct sockaddr_in6 in6 = {};
  in6.sin6_family = AF_INET6;
  in6.sin6_port = htons(443);
  in6.sin6_addr = in6addr_loopback;
  const auto p = parseSockaddr(&in6, sizeof(in6));
  CHECK(p.family == "ipv6");
  CHECK(p.ip == "::1");
  CHECK(p.port == 443);
  CHECK(p.display == "[::1]:443");
}

TEST_CASE("unix carries the path", "[sockaddr]") {
  struct sockaddr_un un = {};
  un.sun_family = AF_UNIX;
  const char* path = "/tmp/s.sock";
  std::strncpy(un.sun_path, path, sizeof(un.sun_path) - 1);
  const auto len = sizeof(sa_family_t) + std::strlen(path) + 1;
  const auto p = parseSockaddr(&un, len);
  CHECK(p.family == "unix");
  CHECK(p.path == "/tmp/s.sock");
  CHECK(p.display == "unix:/tmp/s.sock");
}

TEST_CASE("unspec decodes for the disconnect idiom", "[sockaddr]") {
  struct sockaddr un = {};
  un.sa_family = AF_UNSPEC;
  const auto p = parseSockaddr(&un, sizeof(un));
  CHECK(p.family == "unspec");
  CHECK(p.display == "family=0");
}
#endif

// sg_tcp.c — TCP loopback checks (see test/fixtures/scenarios/README.md).
// Usage: sg_tcp <dir> (dir unused; kept for driver uniformity).
// - bind 127.0.0.1:0 and ::1:0, listen, print the ephemeral ports.
// - blocking connect to each (ok:true, initiated:true).
// - non-blocking connect to 192.0.2.1:80 (TEST-NET-1, unroutable: with a
//   default route the SYN goes unanswered and connect returns -EINPROGRESS,
//   recorded as initiated:true). Asserts EINPROGRESS loudly: silently
//   accepting another outcome would bake environment flakiness into goldens.
// - bind 127.0.0.1:0, close it, connect to the freed port (ECONNREFUSED,
//   recorded as initiated:false, ok:false). Asserts ECONNREFUSED likewise.
// - UDP-disconnect idiom: connect a datagram socket, then connect it to
//   AF_UNSPEC (the resolver's unconnect, always succeeds). Recorded as
//   net.disconnect, never as a connect to nothing.
// Exits nonzero with perror on any setup failure.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail(const char* what) {
  perror(what);
  return 1;
}

static int listen_ephemeral(int domain, const char* ip, unsigned short* portOut) {
  int s = socket(domain, SOCK_STREAM, 0);
  if (s < 0) {
    return -1;
  }
  if (domain == AF_INET) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 5) != 0) {
      close(s);
      return -1;
    }
    socklen_t len = sizeof(a);
    if (getsockname(s, (struct sockaddr*)&a, &len) != 0) {
      close(s);
      return -1;
    }
    *portOut = ntohs(a.sin_port);
  } else {
    struct sockaddr_in6 a6;
    memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6;
    a6.sin6_addr = in6addr_loopback;
    a6.sin6_port = 0;
    if (bind(s, (struct sockaddr*)&a6, sizeof(a6)) != 0 || listen(s, 5) != 0) {
      close(s);
      return -1;
    }
    socklen_t len = sizeof(a6);
    if (getsockname(s, (struct sockaddr*)&a6, &len) != 0) {
      close(s);
      return -1;
    }
    *portOut = ntohs(a6.sin6_port);
  }
  printf("listen %s port %u\n", ip, *portOut);
  return s;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  unsigned short p4 = 0, p6 = 0;
  int srv4 = listen_ephemeral(AF_INET, "127.0.0.1", &p4);
  if (srv4 < 0 || p4 == 0) {
    return fail("listen v4");
  }
  int srv6 = listen_ephemeral(AF_INET6, "::1", &p6);
  if (srv6 < 0 || p6 == 0) {
    return fail("listen v6");
  }

  struct sockaddr_in dst4;
  memset(&dst4, 0, sizeof(dst4));
  dst4.sin_family = AF_INET;
  dst4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  dst4.sin_port = htons(p4);
  int c4 = socket(AF_INET, SOCK_STREAM, 0);
  if (c4 < 0 || connect(c4, (struct sockaddr*)&dst4, sizeof(dst4)) != 0) {
    return fail("blocking connect v4");
  }
  close(c4);

  struct sockaddr_in6 dst6;
  memset(&dst6, 0, sizeof(dst6));
  dst6.sin6_family = AF_INET6;
  dst6.sin6_addr = in6addr_loopback;
  dst6.sin6_port = htons(p6);
  int c6 = socket(AF_INET6, SOCK_STREAM, 0);
  if (c6 < 0 || connect(c6, (struct sockaddr*)&dst6, sizeof(dst6)) != 0) {
    return fail("blocking connect v6");
  }
  close(c6);

  // Unroutable TEST-NET-1 address: non-blocking connect must not complete.
  int nb = socket(AF_INET, SOCK_STREAM, 0);
  if (nb < 0) {
    return fail("socket nb");
  }
  int fl = fcntl(nb, F_GETFL, 0);
  if (fl < 0 || fcntl(nb, F_SETFL, fl | O_NONBLOCK) != 0) {
    return fail("nonblock");
  }
  struct sockaddr_in dead;
  memset(&dead, 0, sizeof(dead));
  dead.sin_family = AF_INET;
  dead.sin_port = htons(80);
  if (inet_pton(AF_INET, "192.0.2.1", &dead.sin_addr) != 1) {
    return fail("pton");
  }
  errno = 0;
  if (connect(nb, (struct sockaddr*)&dead, sizeof(dead)) == 0 || errno != EINPROGRESS) {
    fprintf(stderr, "nonblocking connect: want EINPROGRESS, got r=0/errno=%d\n", errno);
    return 1;
  }
  printf("nonblocking connect: EINPROGRESS as expected\n");
  close(nb);

  // Freed ephemeral port: guaranteed closed (modulo a µs-scale steal race).
  int tmp = socket(AF_INET, SOCK_STREAM, 0);
  if (tmp < 0) {
    return fail("socket tmp");
  }
  struct sockaddr_in taddr;
  memset(&taddr, 0, sizeof(taddr));
  taddr.sin_family = AF_INET;
  taddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  taddr.sin_port = 0;
  if (bind(tmp, (struct sockaddr*)&taddr, sizeof(taddr)) != 0) {
    return fail("bind tmp");
  }
  socklen_t tlen = sizeof(taddr);
  if (getsockname(tmp, (struct sockaddr*)&taddr, &tlen) != 0) {
    return fail("getsockname tmp");
  }
  close(tmp);
  int cr = socket(AF_INET, SOCK_STREAM, 0);
  if (cr < 0) {
    return fail("socket closed");
  }
  errno = 0;
  if (connect(cr, (struct sockaddr*)&taddr, sizeof(taddr)) == 0 || errno != ECONNREFUSED) {
    fprintf(stderr, "closed-port connect: want ECONNREFUSED, got errno=%d\n", errno);
    return 1;
  }
  printf("closed-port connect: ECONNREFUSED as expected\n");
  close(cr);

  // UDP-disconnect idiom (resolver unconnect): connect a datagram socket
  // to the loopback listener, then connect the same fd to AF_UNSPEC.
  // The first is an ordinary net.connect; the second has no peer and is
  // recorded as net.disconnect (ok:true). Both assert loudly.
  int dg = socket(AF_INET, SOCK_DGRAM, 0);
  if (dg < 0) {
    return fail("socket udp");
  }
  if (connect(dg, (struct sockaddr*)&dst4, sizeof(dst4)) != 0) {
    return fail("udp connect");
  }
  struct sockaddr unspec;
  memset(&unspec, 0, sizeof(unspec));
  unspec.sa_family = AF_UNSPEC;
  if (connect(dg, &unspec, sizeof(sa_family_t)) != 0) {
    return fail("udp disconnect");
  }
  printf("udp disconnect: ok\n");
  close(dg);

  close(srv4);
  close(srv6);
  printf("tcp: done\n");
  return 0;
}

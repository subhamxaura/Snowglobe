// sg_sockets.c — AF_UNIX checks without an interpreter (see
// test/fixtures/scenarios/README.md). Usage: sg_sockets <dir>.
// Stream bind+connect on <dir>/sock and on the fixed abstract name
// "sg-snowglobe-abstract" (fixed: abstract names must be deterministic),
// plus a datagram sendto with destination. No network. Exits nonzero
// with perror on any failure.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fail(const char* what) {
  perror(what);
  return 1;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: sg_sockets <dir>\n");
    return 2;
  }
  char path[1080];
  snprintf(path, sizeof(path), "%s/sock", argv[1]);

  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  if (srv < 0) {
    return fail("socket");
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path);
  if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    return fail("bind fs");
  }
  if (listen(srv, 1) != 0) {
    return fail("listen");
  }
  int cli = socket(AF_UNIX, SOCK_STREAM, 0);
  if (cli < 0) {
    return fail("socket cli");
  }
  if (connect(cli, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    return fail("connect fs");
  }
  int conn = accept(srv, NULL, NULL);
  if (conn < 0) {
    return fail("accept");
  }
  if (send(conn, "ping", 4, 0) != 4) {
    return fail("send");
  }
  close(cli);
  close(conn);
  close(srv);

  static const char name[] = "sg-snowglobe-abstract";
  int srv2 = socket(AF_UNIX, SOCK_STREAM, 0);
  if (srv2 < 0) {
    return fail("socket2");
  }
  struct sockaddr_un abs;
  memset(&abs, 0, sizeof(abs));
  abs.sun_family = AF_UNIX;
  abs.sun_path[0] = '\0';
  memcpy(&abs.sun_path[1], name, sizeof(name));
  const unsigned abslen = (unsigned)offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(name);
  if (bind(srv2, (struct sockaddr*)&abs, abslen) != 0) {
    return fail("bind abstract");
  }
  if (listen(srv2, 1) != 0) {
    return fail("listen2");
  }
  int cli2 = socket(AF_UNIX, SOCK_STREAM, 0);
  if (cli2 < 0) {
    return fail("socket cli2");
  }
  if (connect(cli2, (struct sockaddr*)&abs, abslen) != 0) {
    return fail("connect abstract");
  }
  close(cli2);
  close(srv2);

  char pa[1080], pb[1080];
  snprintf(pa, sizeof(pa), "%s/dg-a", argv[1]);
  snprintf(pb, sizeof(pb), "%s/dg-b", argv[1]);
  int a = socket(AF_UNIX, SOCK_DGRAM, 0);
  int b = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (a < 0 || b < 0) {
    return fail("socket dgram");
  }
  struct sockaddr_un da, db;
  memset(&da, 0, sizeof(da));
  memset(&db, 0, sizeof(db));
  da.sun_family = AF_UNIX;
  db.sun_family = AF_UNIX;
  strcpy(da.sun_path, pa);
  strcpy(db.sun_path, pb);
  if (bind(a, (struct sockaddr*)&da, sizeof(da)) != 0) {
    return fail("bind dg-a");
  }
  if (bind(b, (struct sockaddr*)&db, sizeof(db)) != 0) {
    return fail("bind dg-b");
  }
  if (sendto(a, "hi", 2, 0, (struct sockaddr*)&db, sizeof(db)) != 2) {
    return fail("sendto");
  }
  close(a);
  close(b);
  printf("sockets: done\n");
  return 0;
}

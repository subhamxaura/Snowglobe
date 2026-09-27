#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>

#include "tracer/ptrace/open_flags.hpp"

TEST_CASE("classify open flags", "[open]") {
  using snowglobe::tracer::classifyOpenFlags;

  const auto ro = classifyOpenFlags(O_RDONLY, false);
  CHECK(!ro.write);
  CHECK(!ro.create);
  CHECK(!ro.trunc);
  CHECK(!ro.tmpfile);
  CHECK(!ro.dirOrPath);

  const auto wct = classifyOpenFlags(O_WRONLY | O_CREAT | O_TRUNC, false);
  CHECK(wct.write);
  CHECK(wct.create);
  CHECK(wct.trunc);
  CHECK(!wct.tmpfile);
  CHECK(!wct.dirOrPath);

  const auto rw = classifyOpenFlags(O_RDWR, false);
  CHECK(rw.write);
  CHECK(!rw.create);

  const auto creatOnly = classifyOpenFlags(O_CREAT | O_RDONLY, false);
  CHECK(creatOnly.create);
  CHECK(!creatOnly.write);

  // creat(2) emulation: no flags argument.
  const auto creat = classifyOpenFlags(0, true);
  CHECK(creat.write);
  CHECK(creat.create);
  CHECK(creat.trunc);

  // Directory / path handles carry no content: skipped by default.
  const auto opath = classifyOpenFlags(O_PATH | O_RDONLY, false);
  CHECK(opath.dirOrPath);
  CHECK(!opath.write);
  const auto odir = classifyOpenFlags(O_DIRECTORY | O_RDONLY, false);
  CHECK(odir.dirOrPath);
  CHECK(!odir.write);

  // O_TMPFILE implies the O_DIRECTORY bit: must still classify as tmpfile.
  const auto tmp = classifyOpenFlags(O_TMPFILE | O_RDWR, false);
  CHECK(tmp.tmpfile);
  CHECK(tmp.write);
  CHECK(!tmp.dirOrPath);
}

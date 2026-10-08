// Unit tests: baseline walk/write/strict-read + exclude rules.
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "diff/baseline.hpp"

namespace {

std::string mkTree() {
  char tmpl[] = "/tmp/sg-base-XXXXXX";
  REQUIRE(::mkdtemp(tmpl) != nullptr);
  return tmpl;
}

void putFile(const std::string& path, const std::string& bytes) {
  FILE* f = ::fopen(path.c_str(), "wb");
  REQUIRE(f != nullptr);
  if (!bytes.empty()) {
    REQUIRE(::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
  }
  REQUIRE(::fclose(f) == 0);
}

void putDir(const std::string& path) {
  REQUIRE(::mkdir(path.c_str(), 0755) == 0);
}

} // namespace

TEST_CASE("isExcluded matches dirs and files, not prefixes", "[diff]") {
  using snowglobe::diff::isExcluded;
  const std::vector<std::string> x = {"node_modules", "gen/out.txt"};
  CHECK(isExcluded("node_modules", x));
  CHECK(isExcluded("node_modules/a/b", x));
  CHECK(!isExcluded("node_modules2", x));
  CHECK(!isExcluded("node_module", x));
  CHECK(isExcluded("gen/out.txt", x));
  CHECK(!isExcluded("gen/out.txt2", x));
  CHECK(!isExcluded("gen", x));
  CHECK(!isExcluded("a", {"", "b"}));
}

TEST_CASE("baseline round-trips a mixed tree", "[diff]") {
  using namespace snowglobe::diff;
  const std::string root = mkTree();
  putFile(root + "/keep.txt", "hello\n");
  putFile(root + "/empty.txt", "");
  putFile(root + "/ünïcode.txt", "u\n");
  putDir(root + "/sub");
  putFile(root + "/sub/nested.txt", "n");
  REQUIRE(::symlink("keep.txt", (root + "/link.txt").c_str()) == 0);
  REQUIRE(::mkfifo((root + "/pipe").c_str(), 0644) == 0);
  putDir(root + "/.git");
  putFile(root + "/.git/objects", "x");
  putDir(root + "/node_modules");
  putFile(root + "/node_modules/dep.js", "y");
  putDir(root + "/.snowglobe");
  putFile(root + "/.snowglobe/r", "z");
  putDir(root + "/skipme");
  putFile(root + "/skipme/f.txt", "w");

  Baseline bl;
  std::string error;
  REQUIRE(writeBaseline(root, root + "/baseline.json", {"skipme"}, bl, error));
  CHECK(bl.project == root);
  CHECK(bl.files.count("keep.txt") == 1);
  CHECK(bl.files.count("empty.txt") == 1);
  CHECK(bl.files.count("ünïcode.txt") == 1);
  CHECK(bl.files.count("sub/nested.txt") == 1);
  CHECK(bl.files.count("link.txt") == 1);
  CHECK(bl.files.at("link.txt").isLink);
  CHECK(bl.files.at("link.txt").target == "keep.txt");
  CHECK(bl.files.count("pipe") == 0);
  CHECK(bl.skippedSpecial == 1);
  CHECK(bl.files.count(".git/objects") == 0);
  CHECK(bl.files.count("node_modules/dep.js") == 0);
  CHECK(bl.files.count(".snowglobe/r") == 0);
  CHECK(bl.files.count("skipme/f.txt") == 0);
  CHECK(bl.files.count("baseline.json") == 0); // own output excluded, even inside the tree

  Baseline back;
  REQUIRE(loadBaseline(root + "/baseline.json", back, error));
  CHECK(back.project == root);
  CHECK(back.files == bl.files);
  CHECK(back.skippedSpecial == bl.skippedSpecial);
  CHECK(back.files.at("empty.txt").size == 0);
  CHECK(back.files.at("empty.txt").shaHex ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("baseline reader rejects corruption loudly", "[diff]") {
  using namespace snowglobe::diff;
  const std::string root = mkTree();
  auto write = [&](const std::string& name, const std::string& bytes) {
    putFile(root + "/" + name, bytes);
    return root + "/" + name;
  };
  Baseline bl;
  std::string error;
  CHECK(!loadBaseline(root + "/nope.json", bl, error));
  CHECK(!error.empty());
  CHECK(!loadBaseline(write("trunc.json", "{\"version\":1,\"project\":\"p\",\"files\":{"), bl,
                      error));
  CHECK(!loadBaseline(write("badver.json", "{\"version\":2,\"project\":\"p\",\"files\":{},"
                                           "\"skipped_special\":0}"),
                      bl, error));
  CHECK(!loadBaseline(write("badtype.json", "{\"version\":1,\"project\":\"p\",\"files\":{"
                                            "\"a\":{\"type\":\"weird\"}},\"skipped_special\":0}"),
                      bl, error));
  CHECK(!loadBaseline(write("trail.json", "{\"version\":1,\"project\":\"p\",\"files\":{},"
                                          "\"skipped_special\":0} trailing"),
                      bl, error));
}

// Unit tests: Hirschberg line diff (correctness properties + edges).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "diff/myers.hpp"

namespace {

// Naive O(NM) edit distance (insert/delete only) as the optimality oracle.
size_t dpDistance(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  std::vector<size_t> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
  for (size_t j = 0; j <= b.size(); ++j) {
    prev[j] = j;
  }
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      cur[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] : 1 + std::min(prev[j], cur[j - 1]);
    }
    prev.swap(cur);
  }
  return prev[b.size()];
}

// Apply regions to a; must reproduce b exactly (reconstruction property).
std::vector<std::string> applyRegions(const std::vector<std::string>& a,
                                      const std::vector<std::string>& b,
                                      const std::vector<snowglobe::diff::Region>& rs) {
  std::vector<std::string> out;
  size_t ai = 0, bi = 0;
  for (const auto& r : rs) {
    // Gap lines align 1:1 (exact regions leave equal-length gaps).
    while (ai < r.aStart) {
      REQUIRE(bi < r.bStart);
      REQUIRE(a[ai] == b[bi]);
      out.push_back(a[ai]);
      ++ai;
      ++bi;
    }
    REQUIRE(ai == r.aStart);
    REQUIRE(bi == r.bStart);
    for (size_t x = r.bStart; x < r.bEnd; ++x) {
      out.push_back(b[x]);
    }
    ai = r.aEnd;
    bi = r.bEnd;
  }
  while (ai < a.size() && bi < b.size()) {
    REQUIRE(a[ai] == b[bi]);
    out.push_back(a[ai]);
    ++ai;
    ++bi;
  }
  REQUIRE(ai == a.size());
  REQUIRE(bi == b.size());
  return out;
}

// Deterministic PRNG (fixed seed: the property test is a golden).
uint64_t rngState = 0x12345678u;
uint64_t nextRand() {
  rngState = rngState * 6364136223846793005ULL + 1442695040888963407ULL;
  return (rngState >> 33) & 0x7fffffffu;
}

} // namespace

TEST_CASE("splitLines edges", "[diff]") {
  using snowglobe::diff::splitLines;
  {
    const auto t = splitLines("");
    CHECK(t.lines.empty());
    CHECK(!t.noNl);
  }
  {
    const auto t = splitLines("a\nb\n");
    CHECK(t.lines == std::vector<std::string>{"a", "b"});
    CHECK(!t.noNl);
  }
  {
    const auto t = splitLines("a\nb");
    CHECK(t.lines == std::vector<std::string>{"a", "b"});
    CHECK(t.noNl);
  }
  {
    // CRLF is content (split on '\n' only): CRLF-only changes stay visible.
    const auto t = splitLines("a\r\nb\r\n");
    CHECK(t.lines == std::vector<std::string>{"a\r", "b\r"});
    CHECK(!t.noNl);
  }
}

TEST_CASE("myers small cases are exact", "[diff]") {
  using snowglobe::diff::myersDiff;
  {
    const auto r = myersDiff({"a", "b"}, {"a", "b"});
    CHECK(r.regions.empty());
    CHECK(!r.fellBack);
    CHECK(r.adds == 0);
    CHECK(r.dels == 0);
  }
  {
    const auto r = myersDiff({}, {"x"});
    REQUIRE(r.regions.size() == 1);
    CHECK(r.regions[0].aStart == 0);
    CHECK(r.regions[0].aEnd == 0);
    CHECK(r.regions[0].bStart == 0);
    CHECK(r.regions[0].bEnd == 1);
    CHECK(r.adds == 1);
    CHECK(r.dels == 0);
  }
  {
    const auto r = myersDiff({"a", "b", "c"}, {"a", "B", "c"});
    CHECK(r.adds == 1);
    CHECK(r.dels == 1);
    CHECK(applyRegions({"a", "b", "c"}, {"a", "B", "c"}, r.regions) ==
          std::vector<std::string>{"a", "B", "c"});
  }
}

TEST_CASE("myers unified patch is byte-exact", "[diff]") {
  using namespace snowglobe::diff;
  const std::vector<std::string> a = {"a", "b", "c"};
  const std::vector<std::string> b = {"a", "B", "c"};
  const DiffResult dr = myersDiff(a, b);
  REQUIRE(!dr.fellBack);
  const std::vector<Hunk> hunks = toUnified(a, b, dr.regions);
  REQUIRE(hunks.size() == 1);
  CHECK(hunks[0].aStart == 0);
  CHECK(hunks[0].aCount == 3);
  CHECK(hunks[0].bStart == 0);
  CHECK(hunks[0].bCount == 3);
  const std::string patch = renderFilePatch("a/f", "b/f", a, b, false, false, hunks);
  CHECK(patch == "--- a/f\n+++ b/f\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n");
}

TEST_CASE("myers no-newline markers", "[diff]") {
  using namespace snowglobe::diff;
  const std::vector<std::string> a = {"a"};
  const std::vector<std::string> b = {"a", "b"};
  const DiffResult dr = myersDiff(a, b);
  const std::vector<Hunk> hunks = toUnified(a, b, dr.regions);
  const std::string patch = renderFilePatch("a/f", "b/f", a, b, true, false, hunks);
  CHECK(patch.find("\\ No newline at end of file\n") != std::string::npos);
}

TEST_CASE("myers properties hold on random small inputs", "[diff]") {
  using namespace snowglobe::diff;
  static const char* pool[] = {"a", "b", "c", "a", "b", "fn(x)", ""};
  for (int t = 0; t < 500; ++t) {
    const size_t n = nextRand() % 13;
    const size_t m = nextRand() % 13;
    std::vector<std::string> a, b;
    for (size_t i = 0; i < n; ++i) {
      a.push_back(pool[nextRand() % 7]);
    }
    for (size_t i = 0; i < m; ++i) {
      b.push_back(pool[nextRand() % 7]);
    }
    const DiffResult r = myersDiff(a, b);
    REQUIRE(!r.fellBack);
    // Reconstruction: regions applied to a yield b.
    CHECK(applyRegions(a, b, r.regions) == b);
    // Optimality: dels+adds equals the DP edit distance.
    CHECK(r.dels + r.adds == dpDistance(a, b));
  }
}

TEST_CASE("myers budget fallback is deterministic", "[diff]") {
  using namespace snowglobe::diff;
  std::vector<std::string> a, b;
  for (int i = 0; i < 6000; ++i) {
    a.push_back("old-" + std::to_string(i));
    b.push_back("new-" + std::to_string(i));
  }
  const DiffResult r = myersDiff(a, b);
  CHECK(r.fellBack);
  REQUIRE(r.regions.size() == 1);
  CHECK(r.regions[0].aStart == 0);
  CHECK(r.regions[0].aEnd == a.size());
  CHECK(r.regions[0].bStart == 0);
  CHECK(r.regions[0].bEnd == b.size());
  CHECK(r.adds == b.size());
  CHECK(r.dels == a.size());
  CHECK(applyRegions(a, b, r.regions) == b);
}

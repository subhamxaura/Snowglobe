#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "trace/jsonl_writer.hpp"
#include "util/sha256.hpp"

TEST_CASE("JsonlWriter hash chain links events", "[trace]") {
  const std::string path = "test_hash_chain_tmp.jsonl";
  std::remove(path.c_str());
  {
    snowglobe::trace::JsonlWriter w(path);
    REQUIRE(w.ok());
    REQUIRE(w.writeEvent(0, "{\"ev\":\"run.meta\"}"));
    REQUIRE(w.writeEvent(1, "{\"ev\":\"proc.exec\"}"));
    CHECK(w.count() == 2);
    CHECK(w.lastHash().size() == 64);
  }
  std::ifstream f(path);
  REQUIRE(f.good());
  std::string l0, l1;
  std::getline(f, l0);
  std::getline(f, l1);
  CHECK(l0.find("\"seq\":0") != std::string::npos);
  CHECK(l0.find("\"prev_hash\":\"0\"") != std::string::npos);
  CHECK(l1.find("\"seq\":1") != std::string::npos);
  // l1.prev_hash must equal l0.hash
  const auto h0pos = l0.find("\"hash\":\"");
  REQUIRE(h0pos != std::string::npos);
  const std::string h0 = l0.substr(h0pos + 8, 64);
  CHECK(l1.find("\"prev_hash\":\"" + h0 + "\"") != std::string::npos);
  // Recompute: hash == sha256(prev_hash || line-minus-hash-field)
  const auto h1pos = l1.find(",\"hash\":\"");
  REQUIRE(h1pos != std::string::npos);
  // Regression tripwire: the line must be a single JSON object (no trailing "}}").
  REQUIRE(l1.back() == '}');
  CHECK(l1.substr(l1.size() - 2) != "}}");
  const std::string withoutHash = l1.substr(0, h1pos) + "}";
  const auto phpos = l1.find("\"prev_hash\":\"");
  const std::string prev = l1.substr(phpos + 13, 64);
  CHECK(snowglobe::util::sha256Hex(prev + withoutHash) == l1.substr(h1pos + 9, 64));
  std::remove(path.c_str());
}

// Unit tests: replay request matching + recorded-run store (ADR-0010).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "replay/replay_match.hpp"
#include "replay/replay_store.hpp"
#include "util/sha256.hpp"

namespace fs = std::filesystem;
using snowglobe::replay::JsonValue;

namespace {

bool parse(const std::string& s, JsonValue& v) {
  std::string err;
  return snowglobe::replay::parseJson(s, v, err);
}

std::string tmpRun() {
  const fs::path p = fs::temp_directory_path() /
                     ("sg-replay-ut-" + std::to_string(::getpid()) + "-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(p / "llm");
  return p.string();
}

void writeFile(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::trunc | std::ios::binary);
  f << data;
  f.flush();
  REQUIRE(f.good());
}

} // namespace

TEST_CASE("replay json parse + canonical", "[replay]") {
  JsonValue v;
  REQUIRE(parse("{\"b\":2,\"a\":[1,true,null,\"x\"]}", v));
  CHECK(snowglobe::replay::canonicalJson(v) == "{\"a\":[1,true,null,\"x\"],\"b\":2}");
  // Escapes round-trip.
  REQUIRE(parse("{\"q\":\"a\\\"b\\n\"}", v));
  CHECK(snowglobe::replay::canonicalJson(v) == "{\"q\":\"a\\\"b\\n\"}");
  // Malformed inputs fail loud.
  CHECK(!parse("{\"a\":}", v));
  CHECK(!parse("{\"a\"", v));
  CHECK(!parse("[1,]", v));
  CHECK(!parse("{\"a\":1} trailing", v));
  CHECK(!parse("\"unterminated", v));
  CHECK(!parse("{\"a\":\"\\x\"}", v));
  // Nesting cap (200 nested arrays must fail, 10 must pass).
  CHECK(parse("[[[[[[[[[[1]]]]]]]]]]", v));
  std::string deep(200, '[');
  deep += std::string(200, ']');
  CHECK(!parse(deep, v));
}

TEST_CASE("replay scrubString ports of normalize.py", "[replay]") {
  using snowglobe::replay::scrubString;
  CHECK(scrubString("/tmp/tmp.AbC123/note.txt") == "$TMP/note.txt");
  CHECK(scrubString("/tmp/tmp.AB_CD/w") == "$TMP/w");
  CHECK(scrubString("http://127.0.0.1:45678/test-data") == "http://127.0.0.1:PORT/test-data");
  CHECK(scrubString("[::1]:9") == "[::1]:PORT");
  CHECK(scrubString("/usr/lib/python3.12/x") == "/usr/lib/python3.V/x");
  CHECK(scrubString("cpython-310") == "cpython-3V");
  CHECK(scrubString("plain words 123") == "plain words 123");
}

TEST_CASE("replay normalizeBody strips volatile keys", "[replay]") {
  using snowglobe::replay::normalizeBody;
  const std::string a =
      "{\"model\":\"m\",\"id\":\"x\",\"user\":\"u\",\"messages\":[{\"role\":\"tool\","
      "\"tool_call_id\":\"call_1\",\"content\":\"wrote 3 bytes to /tmp/tmp.ABC/w/n\"}],"
      "\"created\":123}";
  const std::string b =
      "{\"messages\":[{\"content\":\"wrote 3 bytes to /tmp/tmp.XYZ/w/n\",\"role\":\"tool\","
      "\"tool_call_id\":\"call_9\"}],\"model\":\"m\",\"user\":\"v\",\"created\":999}";
  CHECK(normalizeBody(a) == normalizeBody(b));
  CHECK(normalizeBody(a).find("call_") == std::string::npos);
  CHECK(normalizeBody(a).find("$TMP") != std::string::npos);
  // Non-JSON bodies hash raw (never crash, never empty). NOTE: explicit
  // lengths — plain literals would truncate at the first NUL.
  const std::string bin1("\x00\x01binary", 8);
  const std::string bin2("\x00\x02binary", 8);
  const std::string raw = normalizeBody(bin1);
  CHECK(raw.rfind("raw:", 0) == 0);
  CHECK(raw == normalizeBody(bin1));
  CHECK(raw != normalizeBody(bin2));
}

TEST_CASE("replay requestHash endpoint + method sensitivity", "[replay]") {
  using snowglobe::replay::requestHash;
  const std::string body = "{\"model\":\"m\",\"messages\":[]}";
  const std::string h = requestHash("openai", "POST", "/openai/v1/chat/completions", body);
  CHECK(h.size() == 64);
  CHECK(requestHash("openai", "POST", "/openai/v1/chat/completions", body) == h);
  CHECK(requestHash("anthropic", "POST", "/openai/v1/chat/completions", body) != h);
  CHECK(requestHash("openai", "POST", "/openai/v1/other", body) != h);
  CHECK(requestHash("openai", "POST", "/openai/v1/chat/completions", "{\"model\":\"n\"}") != h);
  // Same path, different verb: a GET health probe must never match the
  // POST that shares its route (P1 — method is load-bearing).
  CHECK(requestHash("openai", "GET", "/openai/v1/chat/completions", body) != h);
  CHECK(requestHash("openai", "HEAD", "/openai/v1/chat/completions", "") !=
        requestHash("openai", "POST", "/openai/v1/chat/completions", ""));
  // Tool-output paths under different tmp dirs still match (volatile).
  const std::string b1 = "{\"model\":\"m\",\"messages\":[{\"content\":\"/tmp/tmp.AAA\"}]}";
  const std::string b2 = "{\"model\":\"m\",\"messages\":[{\"content\":\"/tmp/tmp.BBB\"}]}";
  CHECK(requestHash("openai", "POST", "/p", b1) == requestHash("openai", "POST", "/p", b2));
}

TEST_CASE("replay store load + match + chunks", "[replay]") {
  using snowglobe::replay::ReplayStore;
  const std::string dir = tmpRun();
  writeFile(dir + "/events.jsonl", "{\"ev\":\"llm.response\",\"id\":0,\"status\":200,\"bytes\":11,"
                                   "\"req\":\"llm/0000.req.json\",\"res\":\"llm/0000.res.json\","
                                   "\"idx\":\"llm/0000.res.idx\"}\n");
  writeFile(dir + "/llm/0000.req.json",
            "{\"method\":\"POST\",\"path\":\"/openai/v1/chat/completions\","
            "\"provider\":\"openai\",\"headers\":{},\"body\":\"{\\\"model\\\":\\\"m\\\"}\","
            "\"body_encoding\":\"utf8\"}");
  writeFile(dir + "/llm/0000.res.json", "hello world");
  writeFile(dir + "/llm/0000.res.idx", "{\"off\":0,\"ts_us\":1000}\n{\"off\":6,\"ts_us\":2500}\n");
  ReplayStore st;
  std::string err;
  REQUIRE(st.load(dir, err));
  REQUIRE(st.size() == 1);
  const auto& t = st.turn(0);
  REQUIRE(t.chunks.size() == 2);
  CHECK(t.chunks[0] == "hello ");
  CHECK(t.chunks[1] == "world");
  CHECK(t.resSha256 == snowglobe::util::sha256Hex("hello world"));
  REQUIRE(t.gapsUs.size() == 1);
  CHECK(t.gapsUs[0] == 1500);
  // Primary hash hit consumes the turn...
  auto m = st.match("openai", "POST", "/openai/v1/chat/completions", "{\"model\":\"m\"}");
  CHECK(m.hit);
  CHECK(m.primary);
  CHECK(m.recordedId == 0);
  CHECK(st.served() == 1);
  // ...so the identical retry is a MISS (consumed), logged loudly.
  auto m2 = st.match("openai", "POST", "/openai/v1/chat/completions", "{\"model\":\"m\"}");
  CHECK(!m2.hit);
  CHECK(st.unrecorded() == 1);
  {
    std::ifstream log(dir + "/replay.unrecorded.jsonl", std::ios::binary);
    REQUIRE(static_cast<bool>(log));
    std::ostringstream ss;
    ss << log.rdbuf();
    CHECK(ss.str().find("no-recorded-match") != std::string::npos);
  }
  fs::remove_all(dir);
}

TEST_CASE("replay store fallback is endpoint-ordered", "[replay]") {
  using snowglobe::replay::ReplayStore;
  const std::string dir = tmpRun();
  writeFile(dir + "/events.jsonl", "{\"ev\":\"llm.response\",\"id\":0,\"status\":200,\"bytes\":1,"
                                   "\"req\":\"llm/0000.req.json\",\"res\":\"llm/0000.res.json\","
                                   "\"idx\":\"llm/0000.res.idx\"}\n");
  writeFile(dir + "/llm/0000.req.json",
            "{\"method\":\"POST\",\"path\":\"/openai/v1/chat/completions\","
            "\"provider\":\"openai\",\"headers\":{},\"body\":\"{\\\"model\\\":\\\"m\\\"}\","
            "\"body_encoding\":\"utf8\"}");
  writeFile(dir + "/llm/0000.res.json", "x");
  writeFile(dir + "/llm/0000.res.idx", "");
  ReplayStore st;
  std::string err;
  REQUIRE(st.load(dir, err));
  // Different body, same endpoint -> fallback hit (not primary).
  auto m = st.match("openai", "POST", "/openai/v1/chat/completions", "{\"model\":\"other\"}");
  CHECK(m.hit);
  CHECK(!m.primary);
  CHECK(m.recordedId == 0);
  // Same path + body but different verb -> miss, never fallback (P1:
  // method gates both levels, so a GET probe cannot borrow a POST turn).
  ReplayStore st3;
  REQUIRE(st3.load(dir, err));
  auto m3 = st3.match("openai", "GET", "/openai/v1/chat/completions", "{\"model\":\"m\"}");
  CHECK(!m3.hit);
  CHECK(st3.unrecorded() == 1);
  // Different endpoint -> miss.
  ReplayStore st2;
  REQUIRE(st2.load(dir, err));
  auto m2 = st2.match("anthropic", "POST", "/anthropic/v1/messages", "{}");
  CHECK(!m2.hit);
  CHECK(st2.unrecorded() == 1);
  fs::remove_all(dir);
}

TEST_CASE("replay store consumes identical bodies in order", "[replay]") {
  using snowglobe::replay::ReplayStore;
  // P3: two recorded turns with byte-identical requests. Each incoming
  // match consumes one entry — both serve primary with distinct turn
  // ids; the third identical request is a loud miss (a retry storm
  // drains the queue into 502s, never loops one response).
  const std::string dir = tmpRun();
  writeFile(dir + "/events.jsonl", "{\"ev\":\"llm.response\",\"id\":0,\"status\":200,\"bytes\":1,"
                                   "\"req\":\"llm/0000.req.json\",\"res\":\"llm/0000.res.json\","
                                   "\"idx\":\"llm/0000.res.idx\"}\n"
                                   "{\"ev\":\"llm.response\",\"id\":1,\"status\":200,\"bytes\":1,"
                                   "\"req\":\"llm/0001.req.json\",\"res\":\"llm/0001.res.json\","
                                   "\"idx\":\"llm/0001.res.idx\"}\n");
  for (const char* n : {"0000", "0001"}) {
    writeFile(dir + "/llm/" + n + ".req.json",
              "{\"method\":\"POST\",\"path\":\"/openai/v1/chat/completions\","
              "\"provider\":\"openai\",\"headers\":{},\"body\":\"{\\\"model\\\":\\\"m\\\"}\","
              "\"body_encoding\":\"utf8\"}");
    writeFile(dir + "/llm/" + n + ".res.idx", "");
  }
  writeFile(dir + "/llm/0000.res.json", "x");
  writeFile(dir + "/llm/0001.res.json", "y");
  ReplayStore st;
  std::string err;
  REQUIRE(st.load(dir, err));
  REQUIRE(st.size() == 2);
  auto m1 = st.match("openai", "POST", "/openai/v1/chat/completions", "{\"model\":\"m\"}");
  auto m2 = st.match("openai", "POST", "/openai/v1/chat/completions", "{\"model\":\"m\"}");
  CHECK(m1.hit);
  CHECK(m1.primary);
  CHECK(m2.hit);
  CHECK(m2.primary);
  CHECK(m1.recordedId == 0);
  CHECK(m2.recordedId == 1);
  CHECK(m1.recordedId != m2.recordedId);
  CHECK(st.turn(m1.slot).resBody == "x");
  CHECK(st.turn(m2.slot).resBody == "y");
  CHECK(st.served() == 2);
  auto m3 = st.match("openai", "POST", "/openai/v1/chat/completions", "{\"model\":\"m\"}");
  CHECK(!m3.hit);
  CHECK(st.unrecorded() == 1);
  fs::remove_all(dir);
}

TEST_CASE("replay store rejects torn inputs loudly", "[replay]") {
  using snowglobe::replay::ReplayStore;
  const std::string dir = tmpRun();
  std::string err;
  ReplayStore st;
  // Missing events.jsonl.
  CHECK(!st.load(dir + "-absent", err));
  CHECK(!err.empty());
  // Response event pointing at missing blobs.
  writeFile(dir + "/events.jsonl", "{\"ev\":\"llm.response\",\"id\":0,\"status\":200,\"bytes\":1,"
                                   "\"req\":\"llm/0000.req.json\",\"res\":\"llm/0000.res.json\","
                                   "\"idx\":\"llm/0000.res.idx\"}\n");
  CHECK(!st.load(dir, err));
  // Idx bounds past end of body.
  writeFile(dir + "/llm/0000.req.json",
            "{\"method\":\"POST\",\"path\":\"/p\",\"provider\":\"openai\",\"headers\":{},"
            "\"body\":\"{}\",\"body_encoding\":\"utf8\"}");
  writeFile(dir + "/llm/0000.res.json", "x");
  writeFile(dir + "/llm/0000.res.idx", "{\"off\":0,\"ts_us\":1}\n{\"off\":99,\"ts_us\":2}\n");
  CHECK(!st.load(dir, err));
  // Path escape in the event.
  writeFile(dir + "/events.jsonl", "{\"ev\":\"llm.response\",\"id\":0,\"status\":200,\"bytes\":1,"
                                   "\"req\":\"../evil.json\",\"res\":\"llm/0000.res.json\","
                                   "\"idx\":\"llm/0000.res.idx\"}\n");
  CHECK(!st.load(dir, err));
  // Symlink escape: a blob link pointing outside the run must not load
  // (replaying an untrusted run must not exfiltrate host files) — even
  // when the target holds a well-formed envelope.
  writeFile(dir + "/events.jsonl", "{\"ev\":\"llm.response\",\"id\":0,\"status\":200,\"bytes\":1,"
                                   "\"req\":\"llm/0000.req.json\",\"res\":\"llm/0000.res.json\","
                                   "\"idx\":\"llm/0000.res.idx\"}\n");
  writeFile(dir + "/outside.txt",
            "{\"method\":\"POST\",\"path\":\"/p\",\"provider\":\"openai\",\"headers\":{},"
            "\"body\":\"{}\",\"body_encoding\":\"utf8\"}");
  fs::remove(dir + "/llm/0000.req.json");
  fs::create_symlink(dir + "/outside.txt", dir + "/llm/0000.req.json");
  CHECK(!st.load(dir, err));
  fs::remove_all(dir);
}

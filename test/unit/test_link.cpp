#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

#include "link/event_scan.hpp"
#include "link/link.hpp"
#include "link/tools.hpp"

// Synthetic trace (seq == file order) exercising every rule:
//   0-2   agent startup before any response → pre-turn
//   3-4   HEAD probe → excluded
//   5-6   turn 1 (tool call_9: run_command "do thing")
//   7-9   background child born in turn 1, acts inside it
//   10-12 turn 2 opens (req 2, res 2); child acts at 11 (both agree: 1)
//   13    child acts after res 2 → window says 2, lineage says 1
//   14    straddle root acts in turn 2 → window 2
//   15    child exits → lineage 1 (death bookkept after)
//   16    unknown pid → window 2
namespace {

const char* kOpenAiBody =
    R"json({"choices":[{"message":{"tool_calls":[{"id":"call_9","type":"function",)"
    R"json("function":{"name":"run_command","arguments":"{\"command\": \"do thing\"}"}}]}}]})json";

std::vector<std::string> synthLines() {
  return {
      R"json({"seq":0,"ev":"run.meta","cmd":["agent"]})json",
      R"json({"seq":1,"ev":"proc.start","pid":10,"ppid":1,"tid":10})json",
      R"json({"seq":2,"ev":"proc.exec","pid":10,"tid":10,"path":"/a","argv":["agent"]})json",
      R"json({"seq":3,"ev":"llm.request","id":0,"provider":"anthropic","method":"HEAD","bytes":0,"pid":99,"tid":98})json",
      R"json({"seq":4,"ev":"llm.response","id":0,"status":502,"bytes":2,"req":"llm/0000.req.json","res":"llm/0000.res.json","pid":99,"tid":98})json",
      R"json({"seq":5,"ev":"llm.request","id":1,"provider":"openai","method":"POST","model":"m","bytes":9,"pid":99,"tid":98})json",
      R"json({"seq":6,"ev":"llm.response","id":1,"status":200,"bytes":7,"req":"llm/0001.req.json","res":"llm/0001.res.json","pid":99,"tid":98})json",
      R"json({"seq":7,"ev":"proc.start","pid":20,"ppid":10,"tid":20})json",
      R"json({"seq":8,"ev":"proc.exec","pid":20,"tid":20,"path":"/usr/bin/sh","argv":["sh","-c","do thing"]})json",
      R"json({"seq":9,"ev":"fs.open","pid":20,"tid":20,"path":"/tmp/a","write":true})json",
      R"json({"seq":10,"ev":"llm.request","id":2,"provider":"openai","method":"POST","model":"m","bytes":9,"pid":99,"tid":97})json",
      R"json({"seq":11,"ev":"fs.open","pid":20,"tid":20,"path":"/tmp/b","write":true})json",
      R"json({"seq":12,"ev":"llm.response","id":2,"status":200,"bytes":3,"req":"llm/0002.req.json","res":"llm/0002.res.json","pid":99,"tid":97})json",
      R"json({"seq":13,"ev":"fs.open","pid":20,"tid":20,"path":"/tmp/c","write":true})json",
      R"json({"seq":14,"ev":"fs.open","pid":10,"tid":10,"path":"/tmp/d","write":true})json",
      R"json({"seq":15,"ev":"proc.exit","pid":20,"tid":20,"code":0})json",
      R"json({"seq":16,"ev":"fs.open","pid":30,"tid":30,"path":"/tmp/e","write":true})json",
  };
}

snowglobe::link::BlobReader synthBlobs() {
  return [](const std::string& rel, std::string& out) {
    if (rel == "llm/0001.res.json") {
      out = kOpenAiBody;
      return true;
    }
    return false; // error/unknown bodies: no tools, linkable anyway
  };
}

// Minimal links.json reader (same scanner the linker uses): turn id →
// {seq → basis}, plus tools and unattributed reasons per turn.
struct TurnView {
  std::map<long long, std::string> attr;
  std::vector<std::string> tools;
  std::map<long long, std::string> unattr;
};

long long numField(const std::string& body, size_t obj, const char* key) {
  using snowglobe::link::findField;
  using snowglobe::link::readField;
  size_t v = 0;
  if (!findField(body, obj, key, v)) {
    return -1;
  }
  const auto f = readField(body, v);
  return (f.found && f.isNumber) ? f.num : -1;
}

std::map<long long, TurnView> viewTurns(const std::string& doc) {
  using snowglobe::link::arrayElem;
  using snowglobe::link::findField;
  using snowglobe::link::getStrArray;
  using snowglobe::link::objectStr;
  using snowglobe::link::readField;
  std::map<long long, TurnView> out;
  size_t v = 0;
  if (!findField(doc, 0, "turns", v)) {
    return out;
  }
  for (size_t k = 0;; ++k) {
    const size_t t = arrayElem(doc, v, k);
    if (t == std::string::npos) {
      break;
    }
    TurnView tv;
    const long long id = numField(doc, t, "turn");
    size_t lv = 0;
    if (findField(doc, t, "llm", lv)) {
      size_t tv2 = 0;
      if (findField(doc, lv, "tools", tv2)) {
        getStrArray(doc, tv2, tv.tools);
      }
    }
    size_t av = 0;
    if (findField(doc, t, "attributed", av)) {
      for (size_t j = 0;; ++j) {
        const size_t e = arrayElem(doc, av, j);
        if (e == std::string::npos) {
          break;
        }
        std::string basis;
        objectStr(doc, e, "basis", basis);
        tv.attr[numField(doc, e, "seq")] = basis;
      }
    }
    size_t uv = 0;
    if (findField(doc, t, "unattributed", uv)) {
      for (size_t j = 0;; ++j) {
        const size_t e = arrayElem(doc, uv, j);
        if (e == std::string::npos) {
          break;
        }
        std::string reason;
        objectStr(doc, e, "reason", reason);
        tv.unattr[numField(doc, e, "seq")] = reason;
      }
    }
    out[id] = tv;
  }
  return out;
}

} // namespace

TEST_CASE("linker attributes the synthetic stream by rule", "[link]") {
  using snowglobe::link::buildLinks;
  const auto doc = buildLinks(synthLines(), synthBlobs());
  CHECK(doc.turns == 2);
  CHECK(doc.probesExcluded == 1);
  CHECK(doc.json.rfind("{\"version\":1,\"turns\":[", 0) == 0);

  const auto turns = viewTurns(doc.json);
  REQUIRE(turns.size() == 2);

  // Turn 1: background child (lineage), exec upgraded by argv-match,
  // cross-boundary retention at 13, exit at 15.
  CHECK(turns.at(1).tools == std::vector<std::string>{"call_9"});
  CHECK(turns.at(1).attr == std::map<long long, std::string>{
                                {7, "lineage"},
                                {8, "argv-match"},
                                {9, "lineage"},
                                {11, "lineage"},
                                {13, "lineage"},
                                {15, "lineage"},
                            });
  // Startup evidence is explicit pre-turn, never guessed.
  CHECK(turns.at(1).unattr ==
        std::map<long long, std::string>{{0, "pre-turn"}, {1, "pre-turn"}, {2, "pre-turn"}});

  // Turn 2: straddle root + unknown pid fall back to window.
  CHECK(turns.at(2).tools.empty());
  CHECK(turns.at(2).attr == std::map<long long, std::string>{{14, "window"}, {16, "window"}});
  CHECK(turns.at(2).unattr.empty());

  CHECK(doc.attributed == 8);
  CHECK(doc.basisCounts.at("argv-match") == 1);
  CHECK(doc.basisCounts.at("lineage") == 5);
  CHECK(doc.basisCounts.at("window") == 2);
  CHECK(doc.unattributed == 3);
  CHECK(doc.reasonCounts.at("pre-turn") == 3);

  // Deterministic: same input → byte-identical output.
  CHECK(buildLinks(synthLines(), synthBlobs()).json == doc.json);
}

TEST_CASE("linker res-partition pins in-flight events to the last completed response", "[link]") {
  // R1: span i = [res_i, res_{i+1}). The fs.open at key 3 sits after
  // request 2 (key 2) but before response 2 (key 4): the old
  // "[response, next request)" reading would owner it to turn 2, but the
  // in-flight request moves no boundary — it belongs to turn 1.
  using snowglobe::link::buildLinks;
  const std::vector<std::string> lines = {
      R"json({"seq":0,"ev":"llm.request","id":1,"provider":"openai","method":"POST","model":"m","bytes":9,"pid":99,"tid":98})json",
      R"json({"seq":1,"ev":"llm.response","id":1,"status":200,"bytes":7,"pid":99,"tid":98})json",
      R"json({"seq":2,"ev":"llm.request","id":2,"provider":"openai","method":"POST","model":"m","bytes":9,"pid":99,"tid":97})json",
      R"json({"seq":3,"ev":"fs.open","pid":77,"tid":77,"path":"/tmp/inflight","write":true})json",
      R"json({"seq":4,"ev":"llm.response","id":2,"status":200,"bytes":3,"pid":99,"tid":97})json",
  };
  const auto doc = buildLinks(lines, [](const std::string&, std::string&) { return false; });
  CHECK(doc.turns == 2);
  const auto turns = viewTurns(doc.json);
  REQUIRE(turns.size() == 2);
  CHECK(turns.at(1).attr == std::map<long long, std::string>{{3, "window"}});
  CHECK(turns.at(2).attr.empty());
}

TEST_CASE("argv-match requires token boundaries", "[link]") {
  // R2: "rm" must NOT match "perform_clean" (substring without a
  // boundary), while "/bin/rm" (slash boundary) and "git status" inside
  // "git status --short" (whitespace boundaries) still upgrade to
  // argv-match. Unknown pids keep every exec on the window basis first,
  // so the assertion isolates the upgrade itself.
  using snowglobe::link::buildLinks;
  const char* body =
      R"json({"choices":[{"message":{"tool_calls":[{"id":"call_a","type":"function",)json"
      R"json("function":{"name":"run_command","arguments":"{\"command\": \"rm\"}"}},)json"
      R"json({"id":"call_b","type":"function",)json"
      R"json("function":{"name":"run_command","arguments":"{\"command\": \"git status\"}"}}]}}]})json";
  const std::vector<std::string> lines = {
      R"json({"seq":0,"ev":"llm.request","id":1,"provider":"openai","method":"POST","model":"m","bytes":9,"pid":99,"tid":98})json",
      R"json({"seq":1,"ev":"llm.response","id":1,"status":200,"bytes":7,"req":"llm/0001.res.json","res":"llm/0001.res.json","pid":99,"tid":98})json",
      R"json({"seq":2,"ev":"proc.exec","pid":71,"tid":71,"path":"/usr/bin/sh","argv":["sh","-c","perform_clean"]})json",
      R"json({"seq":3,"ev":"proc.exec","pid":72,"tid":72,"path":"/bin/rm","argv":["/bin/rm"]})json",
      R"json({"seq":4,"ev":"proc.exec","pid":73,"tid":73,"path":"/usr/bin/git","argv":["git","status","--short"]})json",
  };
  auto blobs = [&body](const std::string& rel, std::string& out) {
    if (rel == "llm/0001.res.json") {
      out = body;
      return true;
    }
    return false;
  };
  const auto doc = buildLinks(lines, blobs);
  CHECK(doc.turns == 1);
  const auto turns = viewTurns(doc.json);
  REQUIRE(turns.size() == 1);
  CHECK(turns.at(1).attr == std::map<long long, std::string>{
                                {2, "window"},
                                {3, "argv-match"},
                                {4, "argv-match"},
                            });
}

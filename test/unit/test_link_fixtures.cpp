// Ground-truth linker pins on committed fixtures (already normalised,
// so no normalization helper is needed — the files are read verbatim).
//
// Pin policy (stated per fixture):
// - toy-agent-3turn (125 events, curated, tiny): EXACT turn→seq sets +
//   bases + tools. Exact because the whole signal fits in 12 seqs and the
//   file is curated; any linker change must explain a diff here.
// - claude-code-1-error (735 events, machine-generated): exact turn ids +
//   exact small-turn sets + exact per-turn SIZES + totals + partition
//   completeness + reason/basis vocab. A 190-seq paste would be unreadable
//   and brittle to review; sizes + partition pin the behavior while the
//   byte-compare determinism test pins the bytes.
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "link/event_scan.hpp"
#include "link/link.hpp"
#include "link/tools.hpp"

namespace {

std::vector<std::string> readLines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream f(path, std::ios::binary);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      out.push_back(line);
    }
  }
  return out;
}

std::string fixDir() {
  return SNOWGLOBE_FIXTURES_DIR;
}

snowglobe::link::BlobReader fixBlobs(const std::string& name) {
  return [name](const std::string& rel, std::string& out) {
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
      return false;
    }
    std::ifstream f(fixDir() + "/" + name + "/" + rel, std::ios::binary);
    if (!f) {
      return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
  };
}

struct TurnSets {
  std::map<long long, std::map<long long, std::string>> attr;   // turn → seq → basis
  std::map<long long, std::vector<std::string>> tools;          // turn → tool ids
  std::map<long long, std::map<long long, std::string>> unattr; // turn → seq → reason
};

TurnSets parseDoc(const std::string& doc) {
  using snowglobe::link::arrayElem;
  using snowglobe::link::findField;
  using snowglobe::link::getStrArray;
  using snowglobe::link::objectStr;
  using snowglobe::link::readField;
  TurnSets sets;
  size_t tv = 0;
  if (!findField(doc, 0, "turns", tv)) {
    return sets;
  }
  for (size_t k = 0;; ++k) {
    const size_t t = arrayElem(doc, tv, k);
    if (t == std::string::npos) {
      break;
    }
    size_t iv = 0;
    long long id = -1;
    if (findField(doc, t, "turn", iv)) {
      const auto f = readField(doc, iv);
      if (f.found && f.isNumber) {
        id = f.num;
      }
    }
    size_t lv = 0;
    if (findField(doc, t, "llm", lv)) {
      size_t wv = 0;
      if (findField(doc, lv, "tools", wv)) {
        getStrArray(doc, wv, sets.tools[id]);
      }
    }
    size_t av = 0;
    if (findField(doc, t, "attributed", av)) {
      for (size_t j = 0;; ++j) {
        const size_t e = arrayElem(doc, av, j);
        if (e == std::string::npos) {
          break;
        }
        size_t sv = 0;
        long long seq = -1;
        if (findField(doc, e, "seq", sv)) {
          const auto f = readField(doc, sv);
          if (f.found && f.isNumber) {
            seq = f.num;
          }
        }
        std::string basis;
        objectStr(doc, e, "basis", basis);
        sets.attr[id][seq] = basis;
      }
    }
    size_t uv = 0;
    if (findField(doc, t, "unattributed", uv)) {
      for (size_t j = 0;; ++j) {
        const size_t e = arrayElem(doc, uv, j);
        if (e == std::string::npos) {
          break;
        }
        size_t sv = 0;
        long long seq = -1;
        if (findField(doc, e, "seq", sv)) {
          const auto f = readField(doc, sv);
          if (f.found && f.isNumber) {
            seq = f.num;
          }
        }
        std::string reason;
        objectStr(doc, e, "reason", reason);
        sets.unattr[id][seq] = reason;
      }
    }
  }
  return sets;
}

} // namespace

TEST_CASE("toy-agent-3turn exact turn membership", "[link][fixture]") {
  using snowglobe::link::buildLinks;
  const std::string name = "toy-agent-3turn";
  const auto lines = readLines(fixDir() + "/" + name + "/events.jsonl");
  REQUIRE(lines.size() == 125);
  const auto doc = buildLinks(lines, fixBlobs(name));
  CHECK(doc.turns == 3);
  CHECK(doc.probesExcluded == 0);

  const TurnSets sets = parseDoc(doc.json);
  REQUIRE(sets.attr.size() == 3);
  // Turn 0 ran run_command: the sh -c exec upgrades to argv-match;
  // the child's own start/exec/exit stay lineage.
  CHECK(sets.attr.at(0) == std::map<long long, std::string>{
                               {109, "lineage"},
                               {110, "lineage"},
                               {111, "lineage"},
                               {112, "lineage"},
                               {113, "argv-match"},
                               {114, "lineage"},
                               {115, "lineage"},
                               {116, "window"},
                           });
  CHECK(sets.tools.at(0) == std::vector<std::string>{"call_1"});
  // Turn 1 ran write_file + http_get: the note.txt write lands here.
  CHECK(sets.attr.at(1) ==
        std::map<long long, std::string>{{119, "window"}, {120, "window"}, {121, "window"}});
  CHECK(sets.tools.at(1) == std::vector<std::string>{"call_2", "call_3"});
  // Turn 2 ("done") owns only the tail.
  CHECK(sets.attr.at(2) == std::map<long long, std::string>{{124, "window"}});
  CHECK(sets.tools.at(2).empty());
  // Loader noise before the first response is explicit pre-turn.
  CHECK(doc.unattributed == 107);
  REQUIRE(sets.unattr.at(0).size() == 107);
  for (const auto& kv : sets.unattr.at(0)) {
    CHECK(kv.first <= 106);
    CHECK(kv.second == "pre-turn");
  }
}

TEST_CASE("claude-code-1-error turn sizes and reasons", "[link][fixture]") {
  using snowglobe::link::buildLinks;
  const std::string name = "claude-code-1-error";
  const auto lines = readLines(fixDir() + "/" + name + "/events.jsonl");
  REQUIRE(lines.size() == 735);
  const auto doc = buildLinks(lines, fixBlobs(name));
  CHECK(doc.turns == 11);
  CHECK(doc.probesExcluded == 1); // HEAD probe never a turn

  const TurnSets sets = parseDoc(doc.json);
  std::vector<long long> ids;
  for (const auto& kv : sets.attr) {
    ids.push_back(kv.first);
  }
  CHECK(ids == std::vector<long long>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
  // Exact per-turn sizes (sum 190); error envelopes carry no tools.
  const std::map<long long, size_t> wantSizes = {
      {1, 2},  {2, 2},  {3, 34}, {4, 3},   {5, 10},  {6, 4},
      {7, 27}, {8, 18}, {9, 22}, {10, 27}, {11, 41},
  };
  for (const auto& kv : wantSizes) {
    CHECK(sets.attr.at(kv.first).size() == kv.second);
    CHECK(sets.tools.at(kv.first).empty());
  }
  // Exact small-turn sets (readable slices of the same truth).
  CHECK(sets.attr.at(1) == std::map<long long, std::string>{{525, "window"}, {526, "window"}});
  CHECK(sets.attr.at(2) == std::map<long long, std::string>{{529, "window"}, {530, "window"}});
  CHECK(sets.attr.at(4) ==
        std::map<long long, std::string>{{569, "window"}, {570, "window"}, {571, "window"}});
  CHECK(sets.attr.at(6) == std::map<long long, std::string>{
                               {586, "window"},
                               {587, "window"},
                               {588, "window"},
                               {589, "window"},
                           });
  // Partition completeness: every event is attributed, unattributed, or
  // turn skeleton (12 llm.request + 12 llm.response), disjointly.
  std::set<long long> seen;
  for (const auto& kv : sets.attr) {
    for (const auto& a : kv.second) {
      CHECK(seen.insert(a.first).second); // disjoint across turns
    }
  }
  CHECK(seen.size() == 190);
  CHECK(doc.attributed == 190);
  CHECK(doc.unattributed == 521);
  long long unMax = -1;
  for (const auto& kv : sets.unattr) {
    for (const auto& u : kv.second) {
      CHECK(u.second == "pre-turn"); // every ambiguity has its reason
      unMax = std::max(unMax, u.first);
    }
  }
  CHECK(doc.reasonCounts.at("pre-turn") == 521);
  // All bases come from the known vocabulary (no argv-match: envelopes).
  for (const auto& kv : doc.basisCounts) {
    CHECK((kv.first == "window" || kv.first == "lineage"));
  }
  CHECK(seen.size() + static_cast<size_t>(doc.unattributed) + 24 == lines.size());
}

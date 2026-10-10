#pragma once
// Replay compare (ADR-0010): normalize both runs' events with the
// test/normalize.py rules (ported), compare per category (llm / fs /
// proc / net / exit), render replay-report.json.
//
// Thread ownership: single-threaded (replay epilogue path).
#include <map>
#include <string>
#include <vector>

namespace snowglobe::replay {

struct CategoryResult {
  // identical | diverged | unrecorded (unrecorded: llm only, >0 loud 502s).
  std::string status = "identical";
  std::string detail;
  std::vector<std::string> replayOnly;   // evidence, capped
  std::vector<std::string> originalOnly; // evidence, capped
};

struct ReplayReport {
  std::string original;
  std::string replay;
  long originalExit = -1;
  long replayExit = -1;
  long origTurns = 0;
  long newTurns = 0;
  bool orderMatches = true;
  long unrecorded = 0;
  // LLM serve path split (P2): turns whose replayed request hash equals
  // the recorded one were served exact (primary); the rest were served by
  // the order-preserving endpoint fallback. exact + fallback +
  // unrecorded == newTurns, always.
  long servedExact = 0;
  long servedFallback = 0;
  std::vector<std::string> ignores;
  CategoryResult llm;
  CategoryResult fs;
  CategoryResult proc;
  CategoryResult net;
  CategoryResult exitCat;
  bool clean() const {
    return llm.status == "identical" && fs.status == "identical" && proc.status == "identical" &&
           net.status == "identical" && exitCat.status == "identical";
  }
  std::string toJson() const;
};

struct CompareOptions {
  std::vector<std::string> ignoreFields; // extra top-level fields to strip
};

// Compare origDir (recorded) vs newDir (re-executed). False with err on
// unreadable runs (missing events.jsonl, corrupt lines); behavioral
// differences are NOT errors — they land in the report as diverged.
bool compareReplay(const std::string& origDir, const std::string& newDir,
                   const CompareOptions& opts, ReplayReport& out, std::string& err);

} // namespace snowglobe::replay

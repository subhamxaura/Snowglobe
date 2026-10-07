#pragma once
// Causal linker: attributes traced side effects to LLM turns.
//
// Input: raw events.jsonl lines + a blob reader for llm/ response bodies.
// Output: deterministic links.json (ADR-0006) — a derived sidecar, never a
// rewrite of the hash-chained events.
//
// Turn rule mirrors viewer/lib/model.ts: every non-probe llm.response is a
// turn (INCLUDING errors); probes (HEAD, or model-less with no body) never
// are. A turn spans [its response, next turn's response); the tail runs to
// end of trace (res-partition, R1: the owner is the last *completed*
// response, so an event after request N+1 but before response N+1 still
// belongs to turn N). Ordering is by seq when every event carries one,
// else file order (normalised fixtures strip seq).
//
// Attribution (first hit wins, strongest evidence last upgrades):
//   window:    side effect inside a turn span → confidence high.
//   lineage:   process born inside turn N keeps N across later spans
//              (background children); born pre-turn (straddle) → window.
//   argv-match: proc.exec in turn N whose argv carries a tool command
//              from N's response body → upgrades window/lineage.
//   llm.* events are the turn skeleton (referenced, never attributed).
//   Anything before the first response → unattributed, reason "pre-turn".
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace snowglobe::link {

// BlobReader: fill `out` with the bytes of run-relative `rel`
// (e.g. "llm/0001.res.json"); false = unreadable (tools stay empty).
using BlobReader = std::function<bool(const std::string& rel, std::string& out)>;

struct Attributed {
  long long seq = 0; // order key (seq, or file index without seq)
  std::string basis; // "window" | "lineage" | "argv-match"
  std::string confidence = "high";
};

struct Unattributed {
  long long seq = 0;
  std::string reason; // e.g. "pre-turn"
};

struct TurnLinks {
  long long turn = 0;             // llm id (mirrors viewer Turn.id)
  long long req = 0;              // order key of the llm.request
  long long res = 0;              // order key of the llm.response
  std::vector<std::string> tools; // tool_call ids from the response body
  std::vector<Attributed> attributed;
  std::vector<Unattributed> unattributed;
};

struct LinksDoc {
  std::string json; // canonical links.json bytes (deterministic)
  long long turns = 0;
  long long probesExcluded = 0;
  long long attributed = 0;
  std::map<std::string, long long> basisCounts; // basis → events
  long long unattributed = 0;
  std::map<std::string, long long> reasonCounts; // reason → events
};

// Build the full document from raw event lines (one JSON object each;
// blank lines skipped). Never throws on agent traffic: unparseable lines
// are skipped, unreadable blobs yield empty tool lists.
LinksDoc buildLinks(const std::vector<std::string>& lines, BlobReader blobs);

} // namespace snowglobe::link

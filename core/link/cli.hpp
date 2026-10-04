#pragma once
// snowglobe link — build (or verify with --check) the links.json causal
// sidecar for a run (ADR-0006). Human output → stderr (AGENTS.md §2).
#include <string>
#include <vector>

namespace snowglobe::link {

// args[0] == "link": link <run> [--check]  (run = .sgr dir or events.jsonl)
int cmdLink(const std::vector<std::string>& args);

} // namespace snowglobe::link

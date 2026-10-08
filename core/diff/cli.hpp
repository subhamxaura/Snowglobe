#pragma once
// snowglobe diff / apply / compare over the overlay upper (ADR-0008).
// Human output → stderr; machine output (compare --json) → stdout.
#include <string>
#include <vector>

namespace snowglobe::diff {

// args[0] == "diff":    diff <run> [--stat] [--patch=FILE | --patch FILE]
// args[0] == "apply":   apply <run> [--dry-run] [--yes]
// args[0] == "compare": compare <runA> <runB> [--stat] [--json]
// <run> is a .sgr dir (or a bare events.jsonl file for diff event-mode).
int cmdDiff(const std::vector<std::string>& args);
int cmdApply(const std::vector<std::string>& args);
int cmdCompare(const std::vector<std::string>& args);

} // namespace snowglobe::diff

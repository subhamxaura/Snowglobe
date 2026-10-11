#pragma once
// snowglobe share / verify (ADR-0011). Human output → stderr (AGENTS.md §2).
#include <string>
#include <vector>

namespace snowglobe::share {

// args[0] == "share": share <run> [--out=FILE] [--force] [--with-overlay]
// args[0] == "verify": verify <run-or-bundle> [--extract-to DIR]
int cmdShare(const std::vector<std::string>& args);
int cmdVerify(const std::vector<std::string>& args);

} // namespace snowglobe::share

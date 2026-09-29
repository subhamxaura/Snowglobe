#pragma once
// snowglobe view — serve the embedded viewer + a run dir on localhost.
// Human output → stderr (AGENTS.md §2); returns process exit codes.
#include <string>
#include <vector>

namespace snowglobe::view {

// args[0] == "view": view <run> [--port=N] [--open]  (port 0 = ephemeral)
int cmdView(const std::vector<std::string>& args);

} // namespace snowglobe::view

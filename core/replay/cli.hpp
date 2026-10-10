#pragma once
// snowglobe replay / replay-proxy (ADR-0010). Human output → stderr;
// machine output (the report file, or --json) → stdout (AGENTS.md §2).
#include <string>
#include <vector>

namespace snowglobe::replay {

// args[0] == "replay-proxy": replay-proxy <run> [--realtime]
// Standalone stub server: serves <run>/llm/* blobs on 127.0.0.1:<ephemeral>
// until SIGINT/SIGTERM. Prints the port to stderr, epilogue on shutdown.
int cmdReplayProxy(const std::vector<std::string>& args);

// Recorded run.meta cmd (for `replay` without a -- override). False with
// err when the run has no usable run.meta.
bool loadOrigCmd(const std::string& origDir, std::vector<std::string>& cmd, std::string& err);

// Lightweight replayability gate: the run must carry >=1 llm.request event
// AND >=1 llm/*.req.json envelope. False with err -> exit 69 (replay
// impossible). Torn blobs past this gate fail at proxy load -> exit 70.
bool checkReplayable(const std::string& origDir, std::string& err);

// Compare + report + epilogue for `replay`. Writes
// <newDir>/replay-report.json, prints the epilogue to stderr (and the
// report JSON to stdout when asJson). Returns 0 clean / 65
// diverged-or-unrecorded / 70 internal (compare error or report write
// failure). The agent exit code is compared, never propagated.
int finishReplay(const std::string& origDir, const std::string& newDir,
                 const std::vector<std::string>& ignores, bool asJson);

} // namespace snowglobe::replay

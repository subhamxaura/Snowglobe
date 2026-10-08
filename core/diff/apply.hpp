#pragma once
// Apply engine: validate-all-then-write for project change sets (ADR-0008).
// Thread ownership: stateless free functions, safe from any thread.
#include <string>
#include <vector>

#include "baseline.hpp"
#include "changeset.hpp"

namespace snowglobe::diff {

// One validated operation (phase 2 input) or a reported problem.
struct ApplyOp {
  std::string path;           // project-relative
  char action = '?';          // 'W' write file, 'L' create symlink, 'U' unlink, 'X' remove tree
  std::vector<uint8_t> bytes; // W: content
  unsigned mode = 0644;       // W: upper mode bits
  std::string linkTarget;     // L: target
};

struct ApplyPlan {
  std::vector<ApplyOp> ops;                // validated writes, sorted by path
  std::vector<std::string> alreadyApplied; // skipped, done
  std::vector<std::string> conflicts;      // host vs baseline mismatches
  std::vector<std::string> rejected;       // safety rejections
  bool clean() const {
    return conflicts.empty() && rejected.empty();
  }
};

// Human-readable report (same text for --dry-run and real runs).
std::string renderApplyReport(const ApplyPlan& plan, bool dryRun);

// Phase 1: recompute the project change set and validate every entry
// against the host WITHOUT writing. False + error = infrastructure
// failure (70); conflicts/rejections are a clean plan with clean()==false.
bool planApply(const std::string& projectAbs, const std::string& runDir, ApplyPlan& plan,
               std::string& error);

// Phase 2: execute a clean plan. Validation in phase 1 proved independence,
// so ordering cannot fail; a mid-write IO failure still aborts loudly
// (writes use temp+rename, so the old file survives a torn write).
// False + error on failure.
bool execApply(const std::string& projectAbs, const ApplyPlan& plan, std::string& error);

} // namespace snowglobe::diff

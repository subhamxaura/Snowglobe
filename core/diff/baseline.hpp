#pragma once
// Project baseline for diff/apply (ADR-0008): path -> content identity,
// snapshotted at --isolate start before the agent runs.
// Thread ownership: stateless free functions, safe from any thread.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace snowglobe::diff {

// Default baseline excludes (relative to the project root): repo metadata,
// our own run output (lives under .snowglobe/runs by default), and the
// dependency dir whose churn is noise, not agent behavior.
inline const std::vector<std::string>& defaultBaselineExcludes() {
  static const std::vector<std::string> k = {".git", ".snowglobe", "node_modules"};
  return k;
}

struct BaselineEntry {
  bool isLink = false;
  std::string shaHex; // file: sha256 of the bytes
  std::string target; // link: readlink target, verbatim
  uint64_t size = 0;
  int64_t mtime = 0; // st_mtime, forensics only (never a diff/apply key)
  bool operator==(const BaselineEntry&) const = default;
};

struct Baseline {
  std::string project;                        // absolute, lexically normal
  std::vector<std::string> excludes;          // effective excludes (defaults + custom)
  std::map<std::string, BaselineEntry> files; // rel ("/"-joined) -> entry
  long skippedSpecial = 0;                    // fifos/sockets/devices seen and skipped
};

// True when rel names an excluded path: exact match, or under an excluded
// directory (prefix + '/'). "a/b" never excludes "a/bc".
bool isExcluded(const std::string& rel, const std::vector<std::string>& excludes);

// Walk projectAbs (absolute dir) and write outPath atomically-ish (write +
// rename from a .tmp sibling). Regular files are hashed whole; symlinks
// record targets (never followed, so no escape); anything else counts as
// skippedSpecial. Unreadable files/dirs abort loudly (false + error, never
// a silent hole in the baseline). extraExcludes extend the defaults.
bool writeBaseline(const std::string& projectAbs, const std::string& outPath,
                   const std::vector<std::string>& extraExcludes, Baseline& out,
                   std::string& error);

// Strict reader for our own byte-exact format (version 1, required keys,
// required types). Anything else is a corrupt baseline (false + error),
// never a best-effort parse.
bool loadBaseline(const std::string& path, Baseline& out, std::string& error);

} // namespace snowglobe::diff

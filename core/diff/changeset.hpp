#pragma once
// Change-set computation over overlay uppers + rendering (ADR-0008).
// Thread ownership: stateless free functions, safe from any thread.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "baseline.hpp"

namespace snowglobe::diff {

// Scope of an upper: project (diff/apply source) vs system inventory.
enum class Scope { Project, Etc, Home, FsRw };

std::string scopeName(Scope s);                    // "project" | "etc" | "home" | "fs-rw-N"
std::string scopeLabel(Scope s, size_t fsRwIndex); // "project" | "etc" | "home" | "fs-rw-0" ...

// One classified change. kind: A (added), M (modified), D (deleted),
// R (renamed old->newPath, content-identical), S (special: fifo/socket/
// device present in the upper; shown, never applied).
struct Change {
  Scope scope = Scope::Project;
  size_t fsRwIndex = 0;     // valid when scope == FsRw
  std::string path;         // project- or scope-relative, "/'-joined
  char kind = '?';          // 'A' | 'M' | 'D' | 'R' | 'S'
  std::string newPath;      // kind R: destination rel path
  std::string oldSha;       // M/D/R-old: baseline content hash ("" if unknown)
  std::string oldTarget;    // link-involved M/D: baseline link target ("" if n/a)
  std::string newSha;       // A/M/R-new: upper content hash ("" for D/S)
  bool isLink = false;      // upper entry is a symlink (newSha empty, target in linkTarget)
  std::string linkTarget;   // symlink target (A/M of links)
  bool hostChanged = false; // M/D: host file hash != baseline (hunks withheld)
  bool noOldBytes = false;  // D: old content unrecoverable (header only)
  bool approx = false;      // counts from a budget-fallback whole-file hunk ("~")
  long adds = 0;            // hunk line counts (-1 rendered as "?")
  long dels = 0;
  std::string patch; // unified hunks for this path ("" when none)
};

struct ChangeSet {
  std::vector<Change> changes; // sorted by (scope, path); pending only
  size_t opaqueUnreadable = 0; // upper dirs whose opaque xattr was unreadable
  size_t applied = 0;          // entries already matching the host (suppressed above)
};

// Upper roots present under runDir/overlay (absolute). Missing dirs are
// skipped (a run may lack fs-rw-N). upperOut receives the project upper
// or "" when absent.
struct UpperRoots {
  std::string project;           // .../overlay/upper or ""
  std::string etc;               // .../overlay/etc-upper or ""
  std::string home;              // .../overlay/home-upper or ""
  std::vector<std::string> fsRw; // .../overlay/fs-rw-N (existing only)
};
UpperRoots findUppers(const std::string& runDir);

// Compute the change set. projectAbs is the host project dir (old bytes
// for hunks + host hash checks); baseline is the run's baseline.json
// content (only consulted for Scope::Project). includePatch controls
// hunk computation (stat-only callers pass false).
// Never throws; all IO failures return false + error.
bool computeChanges(const std::string& runDir, const std::string& projectAbs,
                    const Baseline& baseline, bool includePatch, ChangeSet& out,
                    std::string& error);

// Host-independent raw agent set for `compare` (upper-vs-baseline only:
// no host reads, no applied-suppression, no hunks).
bool computeRawChanges(const std::string& runDir, const Baseline& baseline, ChangeSet& out,
                       std::string& error);

// Rendered --stat table (per-path rows + summary) for human output.
std::string renderStat(const ChangeSet& cs);

// Persisted fs/summary.json (machine-readable pending set, deterministic,
// no absolute paths): {"version":1,"changes":[{scope,path,kind,...}],
// "counts":{A,M,D,R,S},"applied":N,"opaque_unreadable":N}.
std::string renderSummaryJson(const ChangeSet& cs);

// compare join of two change sets (both computed with includePatch=false;
// content equality via hashes, no host reads).
struct CompareRow {
  Scope scope = Scope::Project;
  size_t fsRwIndex = 0;
  std::string path;
  bool inA = false;
  bool inB = false;
  bool same = false; // both present with equal kind+content
  std::string note;  // e.g. "A-only", "both, content differs"
};
std::vector<CompareRow> joinChangeSets(const ChangeSet& a, const ChangeSet& b);
std::string renderCompare(const std::vector<CompareRow>& rows);
std::string renderCompareJson(const std::vector<CompareRow>& rows);

// Non-isolate honest mode: event-derived path report (no content, kinds
// carry '?'). eventsPath is the run's events.jsonl.
bool eventPathReport(const std::string& eventsPath, std::string& out, std::string& error);

// Read an upper regular file (also returns its mode bits for apply).
// Follows nothing outside upperAbs (rel comes from our own walk).
bool readUpperBytes(const std::string& upperAbs, const std::string& rel,
                    std::vector<uint8_t>& bytes, unsigned& mode, std::string& error);

} // namespace snowglobe::diff

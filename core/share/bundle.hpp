#pragma once
// Share bundle assembly (ADR-0011): entry collection, secret audit,
// tar+zstd pack, bundle extract. The share-before-pack gate runs the
// audit first, then the same verifier `verify` uses.
//
// Thread ownership: single-threaded (CLI epilogue paths).
#include <string>
#include <vector>

namespace snowglobe::share {

struct AuditHit {
  std::string file; // archive-relative path
  long line = 0;    // 1-based
  std::string kind; // bearer|basic|url-auth|sk|sk-ant|github|akia|slack|query-param
};

// Gather the bundle entry list for runDir in canonical order. Regular
// files (and dirs) only; llm/*.tmp debris is skipped with a note;
// overlay/upper/ is included only with withOverlay (special files —
// whiteouts, fifos, sockets — refuse the whole share, loudly).
// Missing manifest.json/events.jsonl is an error (nothing to share).
bool collectEntries(const std::string& runDir, bool withOverlay, std::vector<std::string>& arcPaths,
                    std::vector<std::string>& notes, std::string& err);

// Scan every collected file for secret shapes (chunked, so multi-GB
// uppers never load whole). Symlinks are not followed for content
// (their targets are unaudited — documented, low risk).
bool auditEntries(const std::string& runDir, const std::vector<std::string>& arcPaths,
                  std::vector<AuditHit>& hits, std::string& err);

// Pack collected entries (read from runDir) into bundlePath:
// normalized tar + zstd level 3, single-threaded one-shot. Refuses to
// clobber an existing bundlePath. Deterministic: same run twice →
// byte-identical output (pinned by test).
bool packBundle(const std::string& runDir, const std::vector<std::string>& arcPaths,
                const std::string& bundlePath, std::string& err);

// Extract bundlePath under destDir (created). Confined names only;
// caps entry count and total bytes (anti-zip-bomb hygiene).
bool extractBundle(const std::string& bundlePath, const std::string& destDir, std::string& err);

// Full share flow: collect -> audit (findings refuse unless force, with
// a loud warning) -> verifyRun gate (never forced) -> pack. Notes (torn
// blob skips etc.) surface for the epilogue; not failures. Audit hits
// surface for the refusal listing (and the --force warning count).
bool shareRun(const std::string& runDir, const std::string& bundlePath, bool force,
              bool withOverlay, std::vector<std::string>& notes, std::vector<AuditHit>& hits,
              std::string& err);

} // namespace snowglobe::share

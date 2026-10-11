#pragma once
// Trace integrity verifier (ADR-0011): hash-chain walk + manifest
// consistency + blob presence. Read-only; shared by `verify` and the
// share-before-pack gate (same code, never two implementations).
//
// Thread ownership: single-threaded (CLI epilogue paths).
#include <string>
#include <vector>

namespace snowglobe::share {

struct VerifyResult {
  bool ok = false;
  std::vector<std::string> errors; // capped, human-readable
  long events = 0;
  std::string lastHash;
};

// Verify an extracted run directory. False only on unreadable inputs
// (missing files); integrity failures land in errors with ok=false.
bool verifyRun(const std::string& runDir, VerifyResult& out, std::string& err);

// Verify a .sgrx bundle: decompress + untar into extractDir (created),
// then verifyRun. The extracted tree is left in place for the caller
// (`verify --extract-to` keeps it; otherwise the caller removes it).
bool verifyBundle(const std::string& bundlePath, const std::string& extractDir, VerifyResult& out,
                  std::string& err);

} // namespace snowglobe::share

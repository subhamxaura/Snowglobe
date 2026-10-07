#pragma once
// Landlock one-shot enforcement shared by --isolate and the doctor
// enforcement probe: read-only base over / plus read-write paths.
// ABI 1 rights only (max compat; doctor may report ABI 3 — we use the
// v1 subset). One-way and inherited: apply LAST, child side only.
#include <string>
#include <vector>

namespace snowglobe::isolate {

// Enforce: RO (read + execute, no write) on /, full RW rights on each
// rwPath (must exist: files or dirs). False + error → caller exits 69.
// Irreversible for this process and its future children.
bool enforceLandlock(const std::vector<std::string>& rwPaths, std::string& error);

// Highest supported ABI, or -errno (same query as probe/landlock.c).
long landlockQueryAbi();

} // namespace snowglobe::isolate

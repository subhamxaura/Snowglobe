#pragma once
// Minimal ustar tar writer/reader for share bundles (ADR-0011).
// Hand-rolled on purpose: the locked dependency set has no tar library,
// and the format subset we need (regular files, dirs, symlinks) is small.
// Metadata is NORMALIZED on write (mtime 0, uid/gid 0, empty
// uname/gname, caller-sorted entry order) so the tar bytes are a pure
// function of the content — byte-determinism is pinned by test.
//
// Thread ownership: stateless free functions; safe from any thread.
// Single-threaded in practice (share/verify epilogue paths).
#include <cstdint>
#include <string>
#include <vector>

namespace snowglobe::share {

struct TarEntry {
  std::string name; // archive path, relative, no leading '/', no ".."
  bool isDir = false;
  bool isSymlink = false;
  std::string linkTarget; // symlink only, stored verbatim
  std::string data;       // regular files only (may hold NULs)
  unsigned mode = 0644;   // file mode bits (dirs: caller passes 0755)
};

// Serialize entries to a tar byte stream. False with err on
// unrepresentable names (>255 bytes even with prefix split).
bool tarWrite(const std::vector<TarEntry>& entries, std::string& out, std::string& err);

// Parse a tar byte stream. False with err on bad magic, bad checksum,
// absolute/parent-escape names, or truncated data (untrusted bundles
// fail loud, never partially).
bool tarRead(const std::string& blob, std::vector<TarEntry>& entries, std::string& err);

} // namespace snowglobe::share

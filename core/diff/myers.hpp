#pragma once
// Line-based diff via Hirschberg LCS (hand-rolled, no new dependency per
// ADR-0008).
// Thread ownership: stateless free functions, safe from any thread.
#include <string>
#include <vector>

namespace snowglobe::diff {

// Split bytes into lines on '\n' (a trailing '\n' ends the last line, it
// does not create an extra empty one). noNl is true when the input is
// non-empty and lacks a trailing newline (rendered as "\ No newline").
struct TextLines {
  std::vector<std::string> lines;
  bool noNl = false;
};
TextLines splitLines(const std::string& bytes);

// One changed region (0-based, half-open): a[aStart,aEnd) replaced by
// b[bStart,bEnd). Pure add: aStart==aEnd; pure delete: bStart==bEnd.
struct Region {
  size_t aStart = 0;
  size_t aEnd = 0;
  size_t bStart = 0;
  size_t bEnd = 0;
};

// Minimal edit regions via Hirschberg LCS, linear space. Slice products
// past the cell budget fall back to a single whole-file replace region
// (still exact counts for full rewrites; marked by fellBack so renderers
// warn instead of silently overstating).
struct DiffResult {
  std::vector<Region> regions;
  bool fellBack = false; // budget/cap fallback: one whole-file region
  size_t adds = 0;       // total added lines (sum over regions)
  size_t dels = 0;       // total deleted lines (sum over regions)
};
DiffResult myersDiff(const std::vector<std::string>& a, const std::vector<std::string>& b);

// Unified hunks with context lines, merged when overlapping or within
// 2*context lines of each other (git-style @@ -a,s +b,s @@).
struct HunkLine {
  char tag = ' '; // ' ' context, '-' old, '+' new
  std::string text;
};
struct Hunk {
  size_t aStart = 0; // 0-based offset into a
  size_t aCount = 0;
  size_t bStart = 0; // 0-based offset into b
  size_t bCount = 0;
  std::vector<HunkLine> body;
};
std::vector<Hunk> toUnified(const std::vector<std::string>& a, const std::vector<std::string>& b,
                            const std::vector<Region>& regions, size_t context = 3);

// Full git-style patch text for one path (headers + hunks + no-newline
// markers). oldPath/newPath are the ---/+++ labels (e.g. a/x, b/x,
// /dev/null for adds/deletes).
std::string renderFilePatch(const std::string& oldLabel, const std::string& newLabel,
                            const std::vector<std::string>& a, const std::vector<std::string>& b,
                            bool aNoNl, bool bNoNl, const std::vector<Hunk>& hunks);

} // namespace snowglobe::diff

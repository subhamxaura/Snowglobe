// Line-based diff: Hirschberg LCS, linear space (ADR-0008: hand-rolled,
// no new dependency). Regions are pure add/delete runs; minimality and
// reconstruction are pinned by property tests in test_myers.cpp.
#include "myers.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <vector>

namespace snowglobe::diff {
namespace {

// Cell-evaluation budget for one myersDiff call (Hirschberg total is
// ~2x the slice product). Exceeded -> whole-file fallback (flagged).
// Ordinary agent edits are tiny cores after trimming; a 2000x2000 core
// costs ~8M. The fallback keeps pathological inputs bounded while its
// counts stay exact for full rewrites.
constexpr size_t kCellBudget = 16000000;

struct Ctx {
  const std::vector<std::string>* a = nullptr;
  const std::vector<std::string>* b = nullptr;
  size_t used = 0;
  bool blown = false;
  std::vector<Region> regions;
  bool spend(size_t n) {
    used += n;
    if (used > kCellBudget) {
      blown = true;
      return false;
    }
    return true;
  }
};

// LCS lengths L[j] = LCS(a[ab,ae), b[bb,bb+j)) for j in [0,blen].
// O((ae-ab)*(be-bb)) time, O(be-bb) space. False when the budget blows.
bool lcsLens(size_t ab, size_t ae, size_t bb, size_t be, std::vector<size_t>& out, Ctx& ctx) {
  const size_t n = ae - ab;
  const size_t m = be - bb;
  if (!ctx.spend(n * m)) {
    return false;
  }
  std::vector<size_t> prev(m + 1, 0), cur(m + 1, 0);
  for (size_t i = 1; i <= n; ++i) {
    cur[0] = 0;
    for (size_t j = 1; j <= m; ++j) {
      if ((*ctx.a)[ab + i - 1] == (*ctx.b)[bb + j - 1]) {
        cur[j] = prev[j - 1] + 1;
      } else {
        cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
      }
    }
    prev.swap(cur);
  }
  out = std::move(prev);
  return true;
}

// LCS lengths against the reversed suffixes, for the backward half:
// back[j] = LCS(a[amid,ae), b[be-j,be)) for j in [0,blen].
bool lcsLensRev(size_t amid, size_t ae, size_t bb, size_t be, std::vector<size_t>& out, Ctx& ctx) {
  const size_t n = ae - amid;
  const size_t m = be - bb;
  if (!ctx.spend(n * m)) {
    return false;
  }
  std::vector<size_t> prev(m + 1, 0), cur(m + 1, 0);
  for (size_t i = 1; i <= n; ++i) {
    cur[0] = 0;
    for (size_t j = 1; j <= m; ++j) {
      if ((*ctx.a)[ae - i] == (*ctx.b)[be - j]) {
        cur[j] = prev[j - 1] + 1;
      } else {
        cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
      }
    }
    prev.swap(cur);
  }
  out = std::move(prev);
  return true;
}

void solve(size_t ab, size_t ae, size_t bb, size_t be, Ctx& ctx);

void solveSplit(size_t ab, size_t ae, size_t bb, size_t be, Ctx& ctx) {
  const size_t n = ae - ab;
  const size_t m = be - bb;
  const size_t amid = ab + n / 2;
  std::vector<size_t> fwd, rev;
  if (!lcsLens(ab, amid, bb, be, fwd, ctx)) {
    return;
  }
  if (!lcsLensRev(amid, ae, bb, be, rev, ctx)) {
    return;
  }
  size_t best = 0;
  size_t bestScore = 0;
  bool first = true;
  for (size_t j = 0; j <= m; ++j) {
    const size_t score = fwd[j] + rev[m - j];
    if (first || score > bestScore) {
      bestScore = score;
      best = j;
      first = false;
    }
  }
  solve(ab, amid, bb, bb + best, ctx);
  if (ctx.blown) {
    return;
  }
  solve(amid, ae, bb + best, be, ctx);
}

void solve(size_t ab, size_t ae, size_t bb, size_t be, Ctx& ctx) {
  if (ctx.blown) {
    return;
  }
  // Strip the common affix first: equal lines are never regions, and the
  // shrink keeps the DP cores (and the budget) small.
  while (ab < ae && bb < be && (*ctx.a)[ab] == (*ctx.b)[bb]) {
    ++ab;
    ++bb;
  }
  while (ae > ab && be > bb && (*ctx.a)[ae - 1] == (*ctx.b)[be - 1]) {
    --ae;
    --be;
  }
  if (ab == ae && bb == be) {
    return; // equal (or empty) slices
  }
  if (ab == ae) {
    ctx.regions.push_back(Region{ab, ae, bb, be}); // pure insertion
    return;
  }
  if (bb == be) {
    ctx.regions.push_back(Region{ab, ae, bb, be}); // pure deletion
    return;
  }
  if (ae - ab == 1) {
    // Single old line: find it in the new slice, if present.
    size_t hit = be;
    for (size_t j = bb; j < be; ++j) {
      if ((*ctx.b)[j] == (*ctx.a)[ab]) {
        hit = j;
        break;
      }
    }
    if (hit < be) {
      if (bb < hit) {
        ctx.regions.push_back(Region{ab, ab, bb, hit});
      }
      if (hit + 1 < be) {
        // After the survivor (ae == ab + 1 here): anchoring at ab would
        // orphan the kept pair from the tiling (found by exhaustive
        // small-case validation: a=["a"] vs b=["b","a","b"]).
        ctx.regions.push_back(Region{ae, ae, hit + 1, be});
      }
      return; // the line itself survives: no region covers it
    }
    ctx.regions.push_back(Region{ab, ae, bb, be}); // replace whole
    return;
  }
  if (be - bb == 1) {
    // Single new line: symmetric.
    size_t hit = ae;
    for (size_t i = ab; i < ae; ++i) {
      if ((*ctx.a)[i] == (*ctx.b)[bb]) {
        hit = i;
        break;
      }
    }
    if (hit < ae) {
      if (ab < hit) {
        ctx.regions.push_back(Region{ab, hit, bb, bb});
      }
      if (hit + 1 < ae) {
        // Past the survivor on both axes (it pairs a[hit] with the
        // single b-line): mirror of the single-old-line anchoring above.
        ctx.regions.push_back(Region{hit + 1, ae, be, be});
      }
      return;
    }
    ctx.regions.push_back(Region{ab, ae, bb, be});
    return;
  }
  solveSplit(ab, ae, bb, be, ctx);
}

} // namespace

TextLines splitLines(const std::string& bytes) {
  TextLines t;
  size_t start = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] == '\n') {
      t.lines.push_back(bytes.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < bytes.size()) {
    t.lines.push_back(bytes.substr(start));
    t.noNl = true;
  } else if (!bytes.empty()) {
    t.noNl = false; // ended exactly on '\n'
  }
  return t;
}

DiffResult myersDiff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  DiffResult out;
  Ctx ctx;
  ctx.a = &a;
  ctx.b = &b;
  solve(0, a.size(), 0, b.size(), ctx);
  if (ctx.blown) {
    out.regions = {Region{0, a.size(), 0, b.size()}};
    out.fellBack = true;
  } else {
    out.regions = std::move(ctx.regions);
    std::sort(out.regions.begin(), out.regions.end(), [](const Region& x, const Region& y) {
      if (x.aStart != y.aStart) {
        return x.aStart < y.aStart;
      }
      return x.bStart < y.bStart;
    });
  }
  for (const Region& r : out.regions) {
    out.dels += r.aEnd - r.aStart;
    out.adds += r.bEnd - r.bStart;
  }
  return out;
}

std::vector<Hunk> toUnified(const std::vector<std::string>& a, const std::vector<std::string>& b,
                            const std::vector<Region>& regions, size_t context) {
  // Linear op walk: equal runs are context, region runs are -/+. Regions
  // are disjoint and sorted; anything outside them aligns 1:1 (verified
  // defensively: a mismatch degrades to a del+ins pair, never a crash).
  struct Op {
    char tag; // ' ', '-', '+'
    size_t ai = 0;
    size_t bi = 0;
  };
  std::vector<Op> ops;
  size_t ai = 0, bi = 0;
  for (const Region& r : regions) {
    while (ai < r.aStart && bi < r.bStart) {
      if (ai >= a.size() || bi >= b.size() || a[ai] != b[bi]) {
        break; // defensive: region data disagrees; emit del+ins below
      }
      ops.push_back(Op{' ', ai, bi});
      ++ai;
      ++bi;
    }
    while (ai < r.aStart) {
      ops.push_back(Op{' ', ai, bi}); // degenerate alignment guard
      ++ai;
    }
    while (bi < r.bStart) {
      ops.push_back(Op{' ', ai, bi});
      ++bi;
    }
    for (size_t x = r.aStart; x < r.aEnd; ++x) {
      ops.push_back(Op{'-', x, bi});
    }
    ai = r.aEnd;
    for (size_t x = r.bStart; x < r.bEnd; ++x) {
      ops.push_back(Op{'+', ai, x});
    }
    bi = r.bEnd;
  }
  while (ai < a.size() && bi < b.size()) {
    if (a[ai] != b[bi]) {
      break; // trailing disagreement past the last region: del+ins pair
    }
    ops.push_back(Op{' ', ai, bi});
    ++ai;
    ++bi;
  }
  while (ai < a.size() && bi >= b.size()) {
    ops.push_back(Op{'-', ai, bi});
    ++ai;
  }
  while (bi < b.size() && ai >= a.size()) {
    ops.push_back(Op{'+', ai, bi});
    ++bi;
  }
  while (ai < a.size() && bi < b.size()) {
    // Unclaimed tail on both sides (should not happen with exact
    // regions): pair them as del+ins so nothing is silently dropped.
    ops.push_back(Op{'-', ai, bi});
    ops.push_back(Op{'+', ai, bi});
    ++ai;
    ++bi;
  }
  // Group into hunks: changed runs ± context, merged across gaps that fit
  // in 2*context shared lines (git behavior).
  std::vector<Hunk> hunks;
  const size_t n = ops.size();
  size_t i = 0;
  auto isChange = [&](size_t k) { return ops[k].tag != ' '; };
  while (i < n) {
    if (!isChange(i)) {
      ++i;
      continue;
    }
    size_t js = i;
    size_t je = i;
    for (;;) {
      // Extend je over the change run plus trailing context, then look
      // ahead: another change within 2*context shared lines joins in.
      size_t k = je;
      while (k < n && !isChange(k)) {
        ++k;
      }
      if (k >= n) {
        je = n;
        break;
      }
      // Shared (context-candidate) lines between je and k:
      const size_t gap = k - je;
      if (gap > 2 * context && je > js) {
        break; // far change: close the hunk (keep trailing ctx below)
      }
      je = k + 1;
    }
    // Hunk body = ops[hs,he): back up to `context` shared lines, forward
    // to `context` shared lines past the last change.
    size_t hs = js;
    size_t back = 0;
    while (hs > 0 && back < context && !isChange(hs - 1)) {
      --hs;
      ++back;
    }
    size_t he = je;
    // Trim trailing shared lines beyond context.
    size_t lastChange = js;
    for (size_t k = js; k < je; ++k) {
      if (isChange(k)) {
        lastChange = k;
      }
    }
    he = std::min(n, lastChange + 1 + context);
    Hunk h;
    h.aStart = ops[hs].ai;
    h.bStart = ops[hs].bi;
    for (size_t k = hs; k < he; ++k) {
      const Op& op = ops[k];
      if (op.tag == ' ') {
        h.body.push_back(HunkLine{' ', a[op.ai]});
      } else if (op.tag == '-') {
        h.body.push_back(HunkLine{'-', a[op.ai]});
      } else {
        h.body.push_back(HunkLine{'+', b[op.bi]});
      }
    }
    h.aCount = 0;
    h.bCount = 0;
    for (const HunkLine& l : h.body) {
      if (l.tag != '+') {
        ++h.aCount;
      }
      if (l.tag != '-') {
        ++h.bCount;
      }
    }
    hunks.push_back(std::move(h));
    i = he;
  }
  return hunks;
}

std::string renderFilePatch(const std::string& oldLabel, const std::string& newLabel,
                            const std::vector<std::string>& a, const std::vector<std::string>& b,
                            bool aNoNl, bool bNoNl, const std::vector<Hunk>& hunks) {
  std::ostringstream ss;
  ss << "--- " << oldLabel << "\n+++ " << newLabel << "\n";
  for (const Hunk& h : hunks) {
    // Git convention: an empty range starts at the line before (0 for an
    // empty old file), otherwise ranges are 1-based; ",1" is omitted.
    const size_t aHead = (h.aCount == 0) ? h.aStart : h.aStart + 1;
    const size_t bHead = (h.bCount == 0) ? h.bStart : h.bStart + 1;
    ss << "@@ -" << aHead;
    if (h.aCount != 1) {
      ss << "," << h.aCount;
    }
    ss << " +" << bHead;
    if (h.bCount != 1) {
      ss << "," << h.bCount;
    }
    ss << " @@\n";
    size_t ai = h.aStart, bi = h.bStart;
    for (const HunkLine& l : h.body) {
      ss << l.tag << l.text << "\n";
      if (l.tag == '-') {
        if (ai + 1 == a.size() && aNoNl) {
          ss << "\\ No newline at end of file\n";
        }
        ++ai;
      } else if (l.tag == '+') {
        if (bi + 1 == b.size() && bNoNl) {
          ss << "\\ No newline at end of file\n";
        }
        ++bi;
      } else {
        // Context line: still a last line when its side ends here (old
        // "a" without NL vs new "a\nb\n" marks after ' a', like git).
        if (ai + 1 == a.size() && aNoNl) {
          ss << "\\ No newline at end of file\n";
        }
        if (bi + 1 == b.size() && bNoNl) {
          ss << "\\ No newline at end of file\n";
        }
        ++ai;
        ++bi;
      }
    }
  }
  (void)a;
  (void)b;
  return ss.str();
}

} // namespace snowglobe::diff

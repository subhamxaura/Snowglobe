// Share bundle assembly (ADR-0011).
#include "bundle.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <zstd.h>

#include "../redact/redact.hpp"
#include "tar.hpp"
#include "verify.hpp"

namespace snowglobe::share {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxBundleBytes = 4ULL << 30; // 4 GiB sanity cap
constexpr size_t kMaxEntries = 1000000;
constexpr size_t kAuditChunk = 1 << 20; // 1 MiB streaming audit window
constexpr size_t kAuditLook = 1 << 13;  // 8 KiB re-read lookahead for shape floors

bool readFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  if (f.bad()) {
    return false;
  }
  out = ss.str();
  return true;
}

bool isReg(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec) && !ec;
}

// Add one archive path (regular file, dir, or symlink) to the entry
// list. Returns false with err on specials (whiteouts/fifos/sockets:
// unrepresentable in ustar — the share refuses rather than shipping a
// silently incomplete upper).
bool pushPath(const fs::path& fsPath, const std::string& arc, std::vector<TarEntry>& tar,
              std::string& err) {
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(fsPath, ec);
  if (ec) {
    err = "cannot stat " + fsPath.string();
    return false;
  }
  TarEntry e;
  e.name = arc;
  if (fs::is_directory(st)) {
    e.isDir = true;
    e.mode = 0755;
    tar.push_back(std::move(e));
    return true;
  }
  if (fs::is_symlink(st)) {
    e.isSymlink = true;
    e.linkTarget = fs::read_symlink(fsPath, ec).string();
    if (ec) {
      err = "cannot read symlink " + fsPath.string();
      return false;
    }
    tar.push_back(std::move(e));
    return true;
  }
  if (fs::is_regular_file(st)) {
    // Modes preserved verbatim (informative; determinism only needs
    // same-run stability, which holds).
    e.mode = static_cast<unsigned>(st.permissions()) & 07777;
    if (!readFile(fsPath.string(), e.data)) {
      err = "cannot read " + fsPath.string();
      return false;
    }
    tar.push_back(std::move(e));
    return true;
  }
  err = "unsupported file type in bundle (whiteout/fifo/socket?): " + fsPath.string();
  return false;
}

} // namespace

bool collectEntries(const std::string& runDir, bool withOverlay, std::vector<std::string>& arcPaths,
                    std::vector<std::string>& notes, std::string& err) {
  arcPaths.clear();
  notes.clear();
  const fs::path root(runDir);
  auto needFile = [&](const std::string& rel) {
    if (!isReg(root / rel)) {
      err = "nothing to share: missing " + (root / rel).string();
      return false;
    }
    arcPaths.push_back(rel);
    return true;
  };
  auto optFile = [&](const std::string& rel) {
    if (isReg(root / rel)) {
      arcPaths.push_back(rel);
    }
    return true;
  };
  if (!needFile("manifest.json")) {
    return false;
  }
  if (!needFile("events.jsonl")) {
    return false;
  }
  // llm/ blobs, sorted; *.tmp debris (torn streams) never ships.
  {
    std::error_code ec;
    if (fs::is_directory(root / "llm", ec) && !ec) {
      std::vector<std::string> names;
      for (const auto& e : fs::directory_iterator(root / "llm", ec)) {
        names.push_back(e.path().filename().string());
      }
      if (ec) {
        err = "cannot list " + (root / "llm").string();
        return false;
      }
      std::sort(names.begin(), names.end());
      for (const std::string& nm : names) {
        if (nm.size() >= 4 && nm.compare(nm.size() - 4, 4, ".tmp") == 0) {
          notes.push_back("skip torn blob llm/" + nm);
          continue;
        }
        if (isReg(root / "llm" / nm)) {
          arcPaths.push_back("llm/" + nm);
        }
      }
    }
  }
  // Optional sidecars (each only when present).
  optFile("links.json");
  optFile("replay-report.json");
  optFile("baseline.json");
  optFile("fs/summary.json");
  if (withOverlay) {
    const fs::path upper = root / "overlay" / "upper";
    std::error_code ec;
    if (!fs::is_directory(upper, ec) || ec) {
      err = "nothing to share with overlay: missing " + upper.string();
      return false;
    }
    // Recursive walk, sorted for determinism (lexical full-path order).
    std::vector<fs::path> todo{upper};
    std::vector<std::pair<fs::path, std::string>> found; // (fs, arc)
    while (!todo.empty()) {
      fs::path dir = todo.back();
      todo.pop_back();
      std::vector<fs::path> kids;
      for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) {
          break; // increment failure also lands here via the post-check
        }
        kids.push_back(e.path());
      }
      if (ec) {
        err = "cannot list " + dir.string();
        return false;
      }
      std::sort(kids.begin(), kids.end());
      for (const fs::path& k : kids) {
        std::error_code rec;
        const std::string rel = fs::relative(k, root, rec).generic_string();
        if (rec) {
          err = "cannot relativize " + k.string();
          return false;
        }
        found.emplace_back(k, rel);
        std::error_code ec2;
        if (fs::is_directory(k, ec2) && !ec2 && !fs::is_symlink(k, ec2)) {
          todo.push_back(k);
        }
      }
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    // Validate every entry up front (specials refuse the whole share —
    // pushPath names the offender); the arc list itself stays the source
    // of truth for pack order.
    for (const auto& [fp, arc] : found) {
      std::vector<TarEntry> probe;
      if (!pushPath(fp, arc, probe, err)) {
        return false;
      }
      arcPaths.push_back(arc);
    }
  }
  return true;
}

bool auditEntries(const std::string& runDir, const std::vector<std::string>& arcPaths,
                  std::vector<AuditHit>& hits, std::string& err) {
  hits.clear();
  const fs::path root(runDir);
  for (const std::string& arc : arcPaths) {
    std::error_code ec;
    const fs::path fp = root / arc;
    // Symlinks carry no content of their own (targets unaudited —
    // documented; the link path itself is event-visible anyway).
    if (fs::is_symlink(fp, ec) || ec) {
      continue;
    }
    if (!isReg(fp)) {
      continue; // dirs have no content
    }
    std::ifstream f(fp, std::ios::binary);
    if (!f) {
      err = "cannot read " + fp.string();
      return false;
    }
    // Streaming windows with lookahead: each 1 MiB owned chunk is
    // scanned with up to 8 KiB of re-read lookahead past its end, so a
    // token starting inside the chunk always sees enough bytes to satisfy
    // the shape length floors (sk- needs 20, AKIA 16, Bearer 8 — all far
    // below 8 KiB). Unbounded tokens (multi-KB JWTs) still report with a
    // truncated span but the right kind and line — existence and location
    // are what the audit promises, never exact lengths. Owned regions
    // partition the file, so every match start is scanned exactly once:
    // no dedupe, no misses (an overlap-carry scheme would miss a floored
    // shape starting just before the edge — pinned by unit).
    long lineBase = 1; // 1-based line number at the owned chunk start
    for (;;) {
      const std::streampos chunkPos = f.tellg();
      std::string chunk(kAuditChunk, '\0');
      f.read(chunk.data(), static_cast<std::streamsize>(kAuditChunk));
      const size_t got = static_cast<size_t>(f.gcount());
      if (f.bad()) {
        err = "cannot read " + fp.string();
        return false;
      }
      chunk.resize(got);
      const bool eof = got < kAuditChunk;
      std::string look;
      if (!eof) {
        look.assign(kAuditLook, '\0');
        f.read(look.data(), static_cast<std::streamsize>(kAuditLook));
        const size_t lgot = static_cast<size_t>(f.gcount());
        if (f.bad()) {
          err = "cannot read " + fp.string();
          return false;
        }
        look.resize(lgot);
        f.clear(); // clear eofbit from a short lookahead read at EOF
        f.seekg(chunkPos + static_cast<std::streamoff>(got));
        if (!f) {
          err = "cannot read " + fp.string();
          return false;
        }
      }
      const std::string window = chunk + look;
      const auto findings = redact::auditSecrets(window);
      for (const auto& fd : findings) {
        if (fd.begin >= chunk.size()) {
          break; // lookahead-only starts belong to the next window
        }
        long line = lineBase;
        for (size_t k = 0; k < fd.begin; ++k) {
          if (chunk[k] == '\n') {
            ++line;
          }
        }
        hits.push_back({arc, line, fd.kind});
      }
      for (char c : chunk) {
        if (c == '\n') {
          ++lineBase;
        }
      }
      if (eof) {
        break;
      }
    }
  }
  return true;
}

bool packBundle(const std::string& runDir, const std::vector<std::string>& arcPaths,
                const std::string& bundlePath, std::string& err) {
  std::error_code ec;
  if (fs::exists(bundlePath, ec)) {
    err = "refusing to clobber existing bundle: " + bundlePath;
    return false;
  }
  std::vector<TarEntry> tar;
  const fs::path root(runDir);
  // Explicit dir entries for llm/ and overlay parents (clean extracts).
  bool needLlmDir = false, needOverlayDir = false, needUpperDir = false, needFsDir = false;
  for (const std::string& arc : arcPaths) {
    if (arc.compare(0, 4, "llm/") == 0) {
      needLlmDir = true;
    }
    if (arc.compare(0, 14, "overlay/upper/") == 0 || arc == "overlay/upper") {
      needOverlayDir = true;
      needUpperDir = true;
    }
    if (arc.compare(0, 3, "fs/") == 0) {
      needFsDir = true;
    }
  }
  if (needFsDir) {
    TarEntry d;
    d.name = "fs";
    d.isDir = true;
    d.mode = 0755;
    tar.push_back(d);
  }
  if (needLlmDir) {
    TarEntry d;
    d.name = "llm";
    d.isDir = true;
    d.mode = 0755;
    tar.push_back(d);
  }
  if (needOverlayDir) {
    TarEntry d1, d2;
    d1.name = "overlay";
    d1.isDir = true;
    d1.mode = 0755;
    d2.name = "overlay/upper";
    d2.isDir = true;
    d2.mode = 0755;
    tar.push_back(d1);
    if (needUpperDir) {
      tar.push_back(d2);
    }
  }
  for (const std::string& arc : arcPaths) {
    if (!pushPath(root / arc, arc, tar, err)) {
      return false;
    }
  }
  std::string tarBytes;
  if (!tarWrite(tar, tarBytes, err)) {
    return false;
  }
  // zstd level 3, single-threaded one-shot: same input bytes always give
  // the same output bytes (multithreaded zstd would not).
  const size_t bound = ZSTD_compressBound(tarBytes.size());
  std::string out(bound, '\0');
  const size_t done = ZSTD_compress(out.data(), bound, tarBytes.data(), tarBytes.size(), 3);
  if (ZSTD_isError(done)) {
    err = std::string("zstd compress: ") + ZSTD_getErrorName(done);
    return false;
  }
  out.resize(done);
  {
    std::ofstream f(bundlePath, std::ios::trunc | std::ios::binary);
    if (!f) {
      err = "cannot write " + bundlePath;
      return false;
    }
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    f.flush();
    if (!f) {
      err = "cannot write " + bundlePath;
      return false;
    }
  }
  return true;
}

bool extractBundle(const std::string& bundlePath, const std::string& destDir, std::string& err) {
  std::string packed;
  if (!readFile(bundlePath, packed)) {
    err = "cannot read " + bundlePath;
    return false;
  }
  const unsigned long long contentSize = ZSTD_getFrameContentSize(packed.data(), packed.size());
  if (contentSize == ZSTD_CONTENTSIZE_UNKNOWN || contentSize == ZSTD_CONTENTSIZE_ERROR) {
    err = "not a single-frame zstd bundle: " + bundlePath;
    return false;
  }
  if (contentSize > kMaxBundleBytes) {
    err = "bundle claims an implausible size (zip-bomb hygiene)";
    return false;
  }
  std::string tarBytes(static_cast<size_t>(contentSize), '\0');
  const size_t done =
      ZSTD_decompress(tarBytes.data(), tarBytes.size(), packed.data(), packed.size());
  if (ZSTD_isError(done)) {
    err = std::string("zstd decompress: ") + ZSTD_getErrorName(done);
    return false;
  }
  tarBytes.resize(done);
  std::vector<TarEntry> entries;
  if (!tarRead(tarBytes, entries, err)) {
    return false;
  }
  if (entries.size() > kMaxEntries) {
    err = "bundle has too many entries (zip-bomb hygiene)";
    return false;
  }
  std::error_code ec;
  fs::create_directories(destDir, ec);
  if (ec) {
    err = "cannot create " + destDir;
    return false;
  }
  {
    // Never merge into a populated tree (a stale extract would mix with
    // the new one and confuse verify/view) — refuse loudly instead.
    bool empty = true;
    for (const auto& e : fs::directory_iterator(destDir, ec)) {
      (void)e;
      empty = false;
      break;
    }
    if (ec || !empty) {
      err = "refusing to extract into non-empty dir: " + destDir;
      return false;
    }
  }
  const fs::path dest = fs::canonical(destDir, ec);
  if (ec) {
    err = "cannot resolve " + destDir;
    return false;
  }
  for (const TarEntry& e : entries) {
    // Belt and braces: tarRead already rejects escapes, but extraction
    // re-checks against the live destination (TOCTOU-grade paranoia is
    // cheap here).
    if (e.name.empty() || e.name[0] == '/') {
      err = "tar: refusing absolute name '" + e.name + "'";
      return false;
    }
    const fs::path target = dest / e.name;
    // Belt and braces: tarRead already rejects escapes, but extraction
    // re-checks element-wise against the live destination root (lexical
    // — never resolving symlinks mid-walk).
    {
      auto ri = dest.begin();
      auto ti = target.begin();
      for (; ri != dest.end() && ti != target.end(); ++ri, ++ti) {
        if (*ri != *ti) {
          err = "tar: entry escapes destination '" + e.name + "'";
          return false;
        }
      }
      if (ri != dest.end()) {
        err = "tar: entry escapes destination '" + e.name + "'";
        return false;
      }
    }
    if (e.isDir) {
      fs::create_directories(target, ec);
      if (ec) {
        err = "cannot create " + target.string();
        return false;
      }
      continue;
    }
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
      err = "cannot create " + target.parent_path().string();
      return false;
    }
    if (e.isSymlink) {
      fs::remove(target, ec);
      fs::create_symlink(e.linkTarget, target, ec);
      if (ec) {
        err = "cannot link " + target.string();
        return false;
      }
      continue;
    }
    // Regular files land 0600 (private by default — traces are
    // recipient-private until they decide otherwise).
    {
      std::ofstream f(target, std::ios::trunc | std::ios::binary);
      if (!f) {
        err = "cannot write " + target.string();
        return false;
      }
      if (!e.data.empty()) {
        f.write(e.data.data(), static_cast<std::streamsize>(e.data.size()));
      }
      f.flush();
      if (!f) {
        err = "cannot write " + target.string();
        return false;
      }
    }
    fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write, ec);
  }
  return true;
}

bool shareRun(const std::string& runDir, const std::string& bundlePath, bool force,
              bool withOverlay, std::vector<std::string>& notes, std::vector<AuditHit>& hits,
              std::string& err) {
  std::vector<std::string> arcs;
  if (!collectEntries(runDir, withOverlay, arcs, notes, err)) {
    return false;
  }
  if (!auditEntries(runDir, arcs, hits, err)) {
    return false;
  }
  if (!hits.empty() && !force) {
    std::ostringstream ss;
    ss << "refusing to share: " << hits.size() << " secret finding(s)";
    err = ss.str();
    return false;
  }
  VerifyResult vr;
  std::string verr;
  if (!verifyRun(runDir, vr, verr) || !vr.ok) {
    err = verr.empty() ? "trace failed verification" : verr;
    if (!vr.ok && !vr.errors.empty()) {
      err += ": " + vr.errors[0];
    }
    return false;
  }
  if (!packBundle(runDir, arcs, bundlePath, err)) {
    return false;
  }
  return true;
}

bool verifyBundle(const std::string& bundlePath, const std::string& extractDir, VerifyResult& out,
                  std::string& err) {
  if (!extractBundle(bundlePath, extractDir, err)) {
    return false;
  }
  return verifyRun(extractDir, out, err);
}

} // namespace snowglobe::share

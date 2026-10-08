// Change-set computation over overlay uppers (ADR-0008).
#include "changeset.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

#include "../link/event_scan.hpp"
#include "../util/sha256.hpp"
#include "../util/string_util.hpp"
#include "myers.hpp"

namespace snowglobe::diff {
namespace {

std::string errnoText(int e) {
  const char* m = ::strerror(e);
  return m != nullptr ? std::string(m) : ("errno " + std::to_string(e));
}

bool readWholeFile(const std::string& path, std::vector<uint8_t>& bytes, std::string& error) {
  FILE* f = ::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    error = "open " + path + ": " + errnoText(errno);
    return false;
  }
  bytes.clear();
  char buf[65536];
  for (;;) {
    const size_t n = ::fread(buf, 1, sizeof(buf), f);
    bytes.insert(bytes.end(), buf, buf + n);
    if (n < sizeof(buf)) {
      if (::ferror(f)) {
        error = "read " + path + ": " + errnoText(errno);
        ::fclose(f);
        return false;
      }
      break;
    }
  }
  ::fclose(f);
  return true;
}

std::string shaOf(const std::vector<uint8_t>& bytes) {
  return snowglobe::util::sha256Hex(bytes);
}

bool hasNul(const std::vector<uint8_t>& bytes) {
  for (uint8_t c : bytes) {
    if (c == 0) {
      return true;
    }
  }
  return false;
}

bool isWhiteout(const struct stat& st) {
  return S_ISCHR(st.st_mode) && major(st.st_rdev) == 0 && minor(st.st_rdev) == 0;
}

// Upper entry kinds straight off lstat (classification comes later).
struct RawUpper {
  bool whiteout = false;
  bool isDir = false;
  bool isLink = false;
  bool isFile = false;
  bool isSpecial = false;
  bool opaque = false; // dirs only
  std::string linkTarget;
  std::vector<uint8_t> bytes; // files
  unsigned mode = 0;
};

bool walkUpper(const std::string& abs, const std::string& rel, std::map<std::string, RawUpper>& out,
               size_t& opaqueUnreadable, const std::vector<std::string>& excludes,
               std::string& error) {
  DIR* d = ::opendir(abs.c_str());
  if (d == nullptr) {
    error = "opendir " + abs + ": " + errnoText(errno);
    return false;
  }
  std::vector<std::string> names;
  for (;;) {
    errno = 0;
    struct dirent* de = ::readdir(d);
    if (de == nullptr) {
      if (errno != 0) {
        error = "readdir " + abs + ": " + errnoText(errno);
        ::closedir(d);
        return false;
      }
      break;
    }
    const std::string n = de->d_name;
    if (n == "." || n == "..") {
      continue;
    }
    names.push_back(n);
  }
  ::closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    const std::string childRel = rel.empty() ? n : rel + "/" + n;
    if (isExcluded(childRel, excludes)) {
      continue; // prunes whole subtrees identically to the baseline walk
    }
    const std::string childAbs = abs + "/" + n;
    struct stat st = {};
    if (::lstat(childAbs.c_str(), &st) != 0) {
      error = "lstat " + childAbs + ": " + errnoText(errno);
      return false;
    }
    RawUpper e;
    if (isWhiteout(st)) {
      e.whiteout = true;
      out[childRel] = std::move(e);
    } else if (S_ISLNK(st.st_mode)) {
      char tgt[4096];
      const ssize_t len = ::readlink(childAbs.c_str(), tgt, sizeof(tgt) - 1);
      if (len < 0) {
        error = "readlink " + childAbs + ": " + errnoText(errno);
        return false;
      }
      e.isLink = true;
      e.linkTarget = std::string(tgt, static_cast<size_t>(len));
      e.mode = st.st_mode & 07777;
      out[childRel] = std::move(e);
    } else if (S_ISREG(st.st_mode)) {
      if (!readWholeFile(childAbs, e.bytes, error)) {
        return false;
      }
      e.isFile = true;
      e.mode = st.st_mode & 0777;
      out[childRel] = std::move(e);
    } else if (S_ISDIR(st.st_mode)) {
      e.isDir = true;
      char val[8] = {};
      const ssize_t len =
          ::getxattr(childAbs.c_str(), "trusted.overlay.opaque", val, sizeof(val) - 1);
      if (len >= 0) {
        e.opaque = (val[0] == 'y');
      } else if (errno != ENODATA && errno != ENOENT) {
        // Unreadable xattr (e.g. EPERM): counted loudly, treated as
        // non-opaque (whiteouts still catch explicit deletions).
        ++opaqueUnreadable;
      }
      out[childRel] = e; // dirs recorded for opaque handling below
      if (!walkUpper(childAbs, childRel, out, opaqueUnreadable, excludes, error)) {
        return false;
      }
    } else {
      e.isSpecial = true;
      e.mode = st.st_mode & 07777;
      out[childRel] = std::move(e);
    }
  }
  return true;
}

// Host-side read for hunk bytes + applied-suppression: returns false only
// on IO errors other than ENOENT (missing = out-of-band, not an error).
bool readHostFile(const std::string& path, std::vector<uint8_t>& bytes, bool& missing,
                  std::string& error) {
  missing = false;
  FILE* f = ::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    if (errno == ENOENT) {
      missing = true;
      bytes.clear();
      return true;
    }
    error = "open " + path + ": " + errnoText(errno);
    return false;
  }
  bytes.clear();
  char buf[65536];
  for (;;) {
    const size_t n = ::fread(buf, 1, sizeof(buf), f);
    bytes.insert(bytes.end(), buf, buf + n);
    if (n < sizeof(buf)) {
      if (::ferror(f)) {
        error = "read " + path + ": " + errnoText(errno);
        ::fclose(f);
        return false;
      }
      break;
    }
  }
  ::fclose(f);
  return true;
}

struct ScopeWalk {
  Scope scope = Scope::Project;
  size_t fsRwIndex = 0;
  std::string upperAbs;
  std::vector<std::string> excludes; // project scope: replayed baseline set
  std::map<std::string, RawUpper> entries;
};

} // namespace

std::string scopeName(Scope s) {
  switch (s) {
  case Scope::Project: return "project";
  case Scope::Etc: return "etc";
  case Scope::Home: return "home";
  case Scope::FsRw: return "fs-rw";
  }
  return "?";
}

std::string scopeLabel(Scope s, size_t fsRwIndex) {
  if (s == Scope::FsRw) {
    return "fs-rw-" + std::to_string(fsRwIndex);
  }
  return scopeName(s);
}

UpperRoots findUppers(const std::string& runDir) {
  UpperRoots u;
  const std::string base = runDir + "/overlay";
  struct stat st = {};
  if (::lstat((base + "/upper").c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    u.project = base + "/upper";
  }
  if (::lstat((base + "/etc-upper").c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    u.etc = base + "/etc-upper";
  }
  if (::lstat((base + "/home-upper").c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    u.home = base + "/home-upper";
  }
  for (size_t i = 0;; ++i) {
    const std::string p = base + "/fs-rw-" + std::to_string(i);
    if (::lstat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      break;
    }
    u.fsRw.push_back(p);
  }
  return u;
}

bool readUpperBytes(const std::string& upperAbs, const std::string& rel,
                    std::vector<uint8_t>& bytes, unsigned& mode, std::string& error) {
  const std::string path = upperAbs + "/" + rel;
  struct stat st = {};
  if (::lstat(path.c_str(), &st) != 0) {
    error = "lstat " + path + ": " + errnoText(errno);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    error = "not a regular file: " + path;
    return false;
  }
  mode = st.st_mode & 0777;
  return readWholeFile(path, bytes, error);
}

struct Pending {
  Change c;
  std::vector<uint8_t> newBytes; // A/M/R-new file content (for hunks)
  std::vector<uint8_t> oldBytes; // M/D host-verified old content
  bool hasOld = false;
  bool hasNew = false;
};

// Upper-vs-baseline classification for one scope walk (no host reads: the
// result is the raw agent change set). Project consults the baseline;
// other scopes have no baseline (present->A, whiteout->D, never M/R).
void classifyWalk(const ScopeWalk& w, const Baseline& baseline, std::vector<Pending>& pending) {
  const bool isProject = (w.scope == Scope::Project);
  // Opaque dirs: baseline files beneath them, absent from the upper walk,
  // are shadowed deletions.
  std::vector<std::string> opaqueDirs;
  for (const auto& [rel, e] : w.entries) {
    if (e.isDir && e.opaque) {
      opaqueDirs.push_back(rel);
    }
  }
  for (const auto& [rel, e] : w.entries) {
    if (e.isDir) {
      continue; // dirs are structural (opaque handled below)
    }
    Pending p;
    p.c.scope = w.scope;
    p.c.fsRwIndex = w.fsRwIndex;
    p.c.path = rel;
    if (e.whiteout) {
      p.c.kind = 'D';
      if (isProject) {
        const auto it = baseline.files.find(rel);
        if (it != baseline.files.end()) {
          if (it->second.isLink) {
            p.c.oldTarget = it->second.target;
          } else {
            p.c.oldSha = it->second.shaHex;
          }
        }
      }
      pending.push_back(std::move(p));
    } else if (e.isSpecial) {
      p.c.kind = 'S';
      pending.push_back(std::move(p));
    } else if (e.isLink) {
      p.c.isLink = true;
      p.c.linkTarget = e.linkTarget;
      if (isProject) {
        const auto it = baseline.files.find(rel);
        if (it == baseline.files.end()) {
          p.c.kind = 'A';
        } else if (it->second.isLink && it->second.target == e.linkTarget) {
          continue; // unchanged link
        } else {
          p.c.kind = 'M';
          if (it->second.isLink) {
            p.c.oldTarget = it->second.target;
          } else {
            p.c.oldSha = it->second.shaHex;
          }
        }
      } else {
        p.c.kind = 'A';
      }
      pending.push_back(std::move(p));
    } else if (e.isFile) {
      const std::string sha = shaOf(e.bytes);
      if (isProject) {
        const auto it = baseline.files.find(rel);
        if (it == baseline.files.end()) {
          p.c.kind = 'A';
          p.c.newSha = sha;
          p.newBytes = e.bytes;
          p.hasNew = true;
        } else if (!it->second.isLink && it->second.shaHex == sha) {
          continue; // unmodified copy-up (e.g. chmod-only): not a change
        } else {
          p.c.kind = 'M';
          if (it->second.isLink) {
            p.c.oldTarget = it->second.target; // link->file type change
          } else {
            p.c.oldSha = it->second.shaHex;
          }
          p.c.newSha = sha;
          p.newBytes = e.bytes;
          p.hasNew = true;
        }
      } else {
        p.c.kind = 'A';
        p.c.newSha = sha;
        p.newBytes = e.bytes;
        p.hasNew = true;
      }
      pending.push_back(std::move(p));
    }
  }
  // Opaque-dir expansion (project scope: baseline-backed; other scopes
  // have no baseline to expand, and their whiteouts already reported D).
  if (isProject) {
    for (const std::string& d : opaqueDirs) {
      const std::string prefix = d + "/";
      for (const auto& [rel, be] : baseline.files) {
        if (rel.compare(0, prefix.size(), prefix) == 0 && w.entries.find(rel) == w.entries.end()) {
          Pending q;
          q.c.scope = w.scope;
          q.c.fsRwIndex = w.fsRwIndex;
          q.c.path = rel;
          q.c.kind = 'D';
          if (be.isLink) {
            q.c.oldTarget = be.target;
          } else {
            q.c.oldSha = be.shaHex;
          }
          pending.push_back(std::move(q));
        }
      }
    }
  }
}

// Rename pairing: D (known content hash, non-empty) + A (same hash) -> R.
// Greedy in sorted path order; leftovers stay D/A. Empty files and links
// never pair (zero bytes prove nothing about intent). Consumed A entries
// are compacted out.
void pairRenames(std::vector<Pending>& pending, const Baseline& baseline) {
  for (size_t i = 0; i < pending.size(); ++i) {
    if (pending[i].c.kind != 'D' || pending[i].c.oldSha.empty()) {
      continue;
    }
    const auto bit = baseline.files.find(pending[i].c.path);
    const uint64_t dSize =
        (bit != baseline.files.end() && !bit->second.isLink) ? bit->second.size : 0;
    if (pending[i].c.scope == Scope::Project && dSize == 0) {
      continue;
    }
    for (size_t j = 0; j < pending.size(); ++j) {
      if (pending[j].c.kind != 'A' || pending[j].c.isLink ||
          pending[j].c.scope != pending[i].c.scope ||
          pending[j].c.fsRwIndex != pending[i].c.fsRwIndex ||
          pending[j].c.newSha != pending[i].c.oldSha || pending[j].newBytes.empty()) {
        continue;
      }
      pending[i].c.kind = 'R';
      pending[i].c.newPath = pending[j].c.path;
      pending[i].c.newSha = pending[j].c.newSha;
      pending[i].newBytes = pending[j].newBytes;
      pending[i].hasNew = true;
      pending[j].c.kind = 0; // consumed
      break;
    }
  }
  std::vector<Pending> kept;
  for (auto& q : pending) {
    if (q.c.kind != 0) {
      kept.push_back(std::move(q));
    }
  }
  pending.swap(kept);
}

void sortPending(std::vector<Pending>& live) {
  std::sort(live.begin(), live.end(), [](const Pending& x, const Pending& y) {
    const int sx = static_cast<int>(x.c.scope);
    const int sy = static_cast<int>(y.c.scope);
    if (sx != sy) {
      return sx < sy;
    }
    if (x.c.fsRwIndex != y.c.fsRwIndex) {
      return x.c.fsRwIndex < y.c.fsRwIndex;
    }
    return x.c.path < y.c.path;
  });
}

// Walk every present upper under runDir (missing scopes skipped: a run
// may lack fs-rw-N). The project scope replays the baseline excludes so
// ignored subtrees stay invisible in diff and apply; ".git" is always
// reviewed anyway (tamper must be visible; apply refuses it), so it is
// stripped from the replayed set. Other scopes have no excludes.
// False + error on IO failure.
bool walkAllUppers(const std::string& runDir, const std::vector<std::string>& baselineExcludes,
                   std::vector<ScopeWalk>& walks, size_t& opaqueUnreadable, std::string& error) {
  const UpperRoots uppers = findUppers(runDir);
  std::vector<std::string> projectExcludes;
  for (const std::string& x : baselineExcludes) {
    if (x != ".git") {
      projectExcludes.push_back(x);
    }
  }
  if (!uppers.project.empty()) {
    walks.push_back({Scope::Project, 0, uppers.project, projectExcludes, {}});
  }
  if (!uppers.etc.empty()) {
    walks.push_back({Scope::Etc, 0, uppers.etc, {}, {}});
  }
  if (!uppers.home.empty()) {
    walks.push_back({Scope::Home, 0, uppers.home, {}, {}});
  }
  for (size_t i = 0; i < uppers.fsRw.size(); ++i) {
    walks.push_back({Scope::FsRw, i, uppers.fsRw[i], {}, {}});
  }
  for (auto& w : walks) {
    if (!walkUpper(w.upperAbs, "", w.entries, opaqueUnreadable, w.excludes, error)) {
      error = "diff: " + error;
      return false;
    }
  }
  return true;
}

bool computeChanges(const std::string& runDir, const std::string& projectAbs,
                    const Baseline& baseline, bool includePatch, ChangeSet& out,
                    std::string& error) {
  out = ChangeSet();
  std::vector<ScopeWalk> walks;
  if (!walkAllUppers(runDir, baseline.excludes, walks, out.opaqueUnreadable, error)) {
    return false;
  }
  std::vector<Pending> pending;
  for (const auto& w : walks) {
    classifyWalk(w, baseline, pending);
  }
  pairRenames(pending, baseline);

  // Host reconciliation (pending view): old bytes from the host project
  // file, accepted only when sha256 == baseline (the dry-run norm: host
  // untouched since the run). Entries already matching the upper state
  // are done (applied) and leave the pending set, counted in
  // ChangeSet::applied so the suppression is visible, never silent.
  std::vector<Pending> live;
  size_t appliedCount = 0;
  for (auto& p : pending) {
    if (p.c.scope != Scope::Project) {
      live.push_back(std::move(p));
      continue;
    }
    const std::string hostPath = projectAbs + "/" + p.c.path;
    if (p.c.kind == 'A') {
      if (p.c.isLink) {
        struct stat st = {};
        if (::lstat(hostPath.c_str(), &st) != 0 && errno == ENOENT) {
          live.push_back(std::move(p)); // absent -> pending add
          continue;
        }
        if (::lstat(hostPath.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
          char tgt[4096];
          const ssize_t len = ::readlink(hostPath.c_str(), tgt, sizeof(tgt) - 1);
          if (len >= 0 && std::string(tgt, static_cast<size_t>(len)) == p.c.linkTarget) {
            ++appliedCount; // same link already here
            continue;
          }
        }
        live.push_back(std::move(p)); // exists and differs -> pending (apply conflicts)
        continue;
      }
      std::vector<uint8_t> host;
      bool missing = false;
      if (!readHostFile(hostPath, host, missing, error)) {
        error = "diff: " + error;
        return false;
      }
      if (missing) {
        live.push_back(std::move(p)); // absent -> pending add
      } else if (shaOf(host) == p.c.newSha) {
        ++appliedCount; // already applied
      } else {
        live.push_back(std::move(p)); // present and different -> pending (apply conflicts)
      }
      continue;
    }
    if (p.c.kind == 'D') {
      if (!p.c.oldTarget.empty()) {
        // Baseline symlink deleted: host must still hold that link (or
        // already be gone). Target lines come from the baseline record,
        // so hunks never depend on host bytes here.
        struct stat st = {};
        if (::lstat(hostPath.c_str(), &st) != 0 && errno == ENOENT) {
          ++appliedCount; // already gone
          continue;
        }
        bool hostLinkMatch = false;
        if (::lstat(hostPath.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
          char tgt[4096];
          const ssize_t len = ::readlink(hostPath.c_str(), tgt, sizeof(tgt) - 1);
          hostLinkMatch = (len >= 0 && std::string(tgt, static_cast<size_t>(len)) == p.c.oldTarget);
        }
        if (!hostLinkMatch) {
          p.c.hostChanged = true; // host moved on (hunks still baseline-sourced)
        }
        live.push_back(std::move(p));
        continue;
      }
      if (p.c.oldSha.empty()) {
        // Unknown old content (dir whiteout, foreign scope): suppress only
        // when the host path is already gone (end state achieved).
        struct stat st = {};
        if (::lstat(hostPath.c_str(), &st) != 0 && errno == ENOENT) {
          ++appliedCount;
          continue;
        }
        live.push_back(std::move(p)); // present: pending, nothing verifiable
        continue;
      }
      std::vector<uint8_t> host;
      bool missing = false;
      if (!readHostFile(hostPath, host, missing, error)) {
        error = "diff: " + error;
        return false;
      }
      if (missing) {
        ++appliedCount; // already gone
        continue;
      }
      if (shaOf(host) == p.c.oldSha) {
        p.oldBytes = std::move(host);
        p.hasOld = true;
        live.push_back(std::move(p));
      } else {
        p.c.hostChanged = true; // host moved on: hunks withheld
        live.push_back(std::move(p));
      }
      continue;
    }
    if (p.c.kind == 'M' || p.c.kind == 'R') {
      if (p.c.isLink) {
        // Upper is a link: verify the host still holds the baseline side
        // (link target, or file content for file->link type changes).
        // Target lines come from records, so hunks never need host bytes.
        bool match = false;
        struct stat st = {};
        if (::lstat(hostPath.c_str(), &st) == 0) {
          if (!p.c.oldTarget.empty() && S_ISLNK(st.st_mode)) {
            char tgt[4096];
            const ssize_t len = ::readlink(hostPath.c_str(), tgt, sizeof(tgt) - 1);
            match = (len >= 0 && std::string(tgt, static_cast<size_t>(len)) == p.c.oldTarget);
          } else if (!p.c.oldSha.empty() && S_ISREG(st.st_mode)) {
            std::vector<uint8_t> host;
            bool missing = false;
            if (!readHostFile(hostPath, host, missing, error)) {
              error = "diff: " + error;
              return false;
            }
            match = !missing && shaOf(host) == p.c.oldSha;
            if (match) {
              p.oldBytes = std::move(host);
              p.hasOld = true;
            }
          }
        }
        if (!match) {
          p.c.hostChanged = true; // host moved on (link hunks still render)
        }
        live.push_back(std::move(p));
        continue;
      }
      if (!p.c.oldTarget.empty()) {
        // Link->file type change: host must still be the baseline link.
        bool match = false;
        struct stat st = {};
        if (::lstat(hostPath.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
          char tgt[4096];
          const ssize_t len = ::readlink(hostPath.c_str(), tgt, sizeof(tgt) - 1);
          match = (len >= 0 && std::string(tgt, static_cast<size_t>(len)) == p.c.oldTarget);
        }
        if (!match) {
          p.c.hostChanged = true;
        }
        live.push_back(std::move(p));
        continue;
      }
      const std::string& checkSha = p.c.oldSha;
      std::vector<uint8_t> host;
      bool missing = false;
      if (!readHostFile(hostPath, host, missing, error)) {
        error = "diff: " + error;
        return false;
      }
      if (!missing && !checkSha.empty() && shaOf(host) == checkSha) {
        p.oldBytes = std::move(host);
        p.hasOld = true;
        live.push_back(std::move(p));
      } else if (!missing && shaOf(host) == p.c.newSha) {
        ++appliedCount; // host already holds the new content
      } else {
        p.c.hostChanged = true;
        live.push_back(std::move(p));
      }
      continue;
    }
    if (p.c.kind == 'S') {
      live.push_back(std::move(p));
      continue;
    }
  }

  // Hunks + counts. Line sources: file sides come from host-verified
  // (old) and upper (new) bytes; link sides are single target lines from
  // the baseline/upper records (host-independent, so link hunks render
  // even with hostChanged set — the warning still shows in --stat).
  for (auto& p : live) {
    Change& c = p.c;
    if (!includePatch) {
      continue;
    }
    if (c.kind == 'S') {
      continue; // inventory only, never hunks
    }
    const bool oldIsTarget = !c.oldTarget.empty();
    const bool newIsTarget = c.isLink && c.kind != 'D';
    const bool needHostOld =
        !oldIsTarget && (c.kind == 'M' || c.kind == 'R' || c.kind == 'D') && !c.oldSha.empty();
    if (needHostOld && !p.hasOld) {
      if (c.kind == 'D') {
        c.noOldBytes = true;
      }
      continue; // M/R without verified old bytes: hunks withheld (see note)
    }
    std::vector<std::string> empty;
    const std::vector<std::string>* oldLines = &empty;
    const std::vector<std::string>* newLines = &empty;
    std::vector<std::string> oldOwned, newOwned, oldTgt, newTgt;
    TextLines ot, nt;
    bool oldBinary = false, newBinary = false, oldNoNl = false, newNoNl = false;
    if (c.kind == 'M' || c.kind == 'R' || c.kind == 'D') {
      if (oldIsTarget) {
        oldTgt.push_back(c.oldTarget);
        oldLines = &oldTgt;
      } else if (p.hasOld) {
        if (hasNul(p.oldBytes)) {
          oldBinary = true;
        } else {
          ot = splitLines(std::string(p.oldBytes.begin(), p.oldBytes.end()));
          oldOwned = ot.lines;
          oldNoNl = ot.noNl;
          oldLines = &oldOwned;
        }
      } else if (c.kind == 'D') {
        c.noOldBytes = true;
        continue;
      } else {
        continue; // M/R file side without old bytes (hostChanged note covers it)
      }
    }
    if (c.kind == 'M' || c.kind == 'R' || c.kind == 'A') {
      if (newIsTarget) {
        newTgt.push_back(c.linkTarget);
        newLines = &newTgt;
      } else if (p.hasNew) {
        if (hasNul(p.newBytes)) {
          newBinary = true;
        } else {
          nt = splitLines(std::string(p.newBytes.begin(), p.newBytes.end()));
          newOwned = nt.lines;
          newNoNl = nt.noNl;
          newLines = &newOwned;
        }
      } else {
        continue; // nothing to diff on the new side
      }
    }
    if (oldBinary || newBinary) {
      c.adds = -1;
      c.dels = -1;
      c.patch = "Binary content (not shown)\n";
      continue;
    }
    const DiffResult dr = myersDiff(*oldLines, *newLines);
    c.adds = static_cast<long>(dr.adds);
    c.dels = static_cast<long>(dr.dels);
    c.approx = dr.fellBack;
    const std::vector<Hunk> hunks = toUnified(*oldLines, *newLines, dr.regions);
    const std::string scopePfx =
        (c.scope == Scope::Project) ? "" : scopeLabel(c.scope, c.fsRwIndex) + "/";
    const std::string& rel = c.path;
    const std::string newRel = (c.kind == 'R') ? c.newPath : c.path;
    const std::string oldLabel = (c.kind == 'A') ? "/dev/null" : "a/" + scopePfx + rel;
    const std::string newLabel = (c.kind == 'D') ? "/dev/null" : "b/" + scopePfx + newRel;
    std::string body =
        renderFilePatch(oldLabel, newLabel, *oldLines, *newLines, oldNoNl, newNoNl, hunks);
    if (dr.fellBack) {
      body = "# snowglobe: diff budget exceeded, whole-file hunk\n" + body;
    }
    c.patch = std::move(body);
  }

  sortPending(live);
  out.changes.clear();
  for (auto& p : live) {
    out.changes.push_back(std::move(p.c));
  }
  out.applied = appliedCount;
  return true;
}

// Host-independent raw agent set for `compare`: upper-vs-baseline only,
// no host reads, no applied-suppression, no hunks (join equality uses
// kind+hashes). D entries keep baseline hashes for equality.
bool computeRawChanges(const std::string& runDir, const Baseline& baseline, ChangeSet& out,
                       std::string& error) {
  out = ChangeSet();
  std::vector<ScopeWalk> walks;
  if (!walkAllUppers(runDir, baseline.excludes, walks, out.opaqueUnreadable, error)) {
    return false;
  }
  std::vector<Pending> pending;
  for (const auto& w : walks) {
    classifyWalk(w, baseline, pending);
  }
  pairRenames(pending, baseline);
  sortPending(pending);
  out.changes.clear();
  for (auto& q : pending) {
    out.changes.push_back(std::move(q.c));
  }
  return true;
}

std::string renderStat(const ChangeSet& cs) {
  std::string s;
  size_t nA = 0, nM = 0, nD = 0, nR = 0, nS = 0;
  for (const Change& c : cs.changes) {
    std::string row;
    row += c.kind;
    row += " ";
    row += scopeLabel(c.scope, c.fsRwIndex);
    row += " ";
    if (c.kind == 'R') {
      row += c.path + " -> " + c.newPath;
    } else {
      row += c.path;
    }
    if (c.kind == 'A' || c.kind == 'M' || c.kind == 'R' || c.kind == 'D') {
      const bool countable = !(c.kind == 'D' && c.noOldBytes) && !(c.adds < 0 || c.dels < 0);
      if (c.isLink && (c.kind == 'A' || c.kind == 'M')) {
        row += " (symlink -> " + c.linkTarget + ")";
      }
      if (countable) {
        row += " +" + std::to_string(c.adds) + (c.approx ? "~" : "") + " -" +
               std::to_string(c.dels) + (c.approx ? "~" : "");
      } else if (c.adds < 0 || c.dels < 0) {
        row += " +? -? (binary)";
      } else if (!(c.isLink && (c.kind == 'A' || c.kind == 'M'))) {
        row += " (old content unavailable)";
      }
    }
    if (c.hostChanged) {
      row += " (host changed since run: hunks withheld)";
    }
    if (c.kind == 'S') {
      row += " (special file: shown, never applied)";
    }
    s += row + "\n";
    switch (c.kind) {
    case 'A': ++nA; break;
    case 'M': ++nM; break;
    case 'D': ++nD; break;
    case 'R': ++nR; break;
    default: ++nS; break;
    }
  }
  if (cs.changes.empty()) {
    s += "no pending changes";
  } else {
    s += std::to_string(cs.changes.size()) + " pending change(s): +" + std::to_string(nA) + " A, " +
         std::to_string(nM) + " M, " + std::to_string(nD) + " D, " + std::to_string(nR) + " R, " +
         std::to_string(nS) + " S";
  }
  if (cs.applied > 0) {
    s += " (" + std::to_string(cs.applied) + " already applied, hidden)";
  }
  if (cs.opaqueUnreadable > 0) {
    s += " (" + std::to_string(cs.opaqueUnreadable) + " opaque xattr(s) unreadable)";
  }
  s += "\n";
  return s;
}

std::vector<CompareRow> joinChangeSets(const ChangeSet& a, const ChangeSet& b) {
  std::map<std::string, const Change*> ma, mb;
  auto key = [](const Change& c) { return scopeLabel(c.scope, c.fsRwIndex) + "\x01" + c.path; };
  for (const Change& c : a.changes) {
    ma[key(c)] = &c;
  }
  for (const Change& c : b.changes) {
    mb[key(c)] = &c;
  }
  std::map<std::string, CompareRow> rows;
  auto contentOf = [](const Change& c) -> std::string {
    if (c.kind == 'R') {
      return std::string("R") + c.newPath + "\x02" + c.newSha;
    }
    if (c.kind == 'D') {
      return std::string("D") + c.oldSha;
    }
    if (c.isLink) {
      return std::string("L") + c.linkTarget;
    }
    return std::string(1, c.kind) + c.newSha;
  };
  for (const auto& [k, ca] : ma) {
    CompareRow r;
    r.scope = ca->scope;
    r.fsRwIndex = ca->fsRwIndex;
    r.path = ca->path;
    r.inA = true;
    const auto it = mb.find(k);
    if (it == mb.end()) {
      r.inB = false;
      r.same = false;
      r.note = "A-only";
    } else {
      r.inB = true;
      const Change* cb = it->second;
      if (ca->kind == cb->kind && contentOf(*ca) == contentOf(*cb)) {
        r.same = true;
        r.note = "both, same";
      } else {
        r.same = false;
        r.note = "both, differ";
      }
    }
    rows[k] = r;
  }
  for (const auto& [k, cb] : mb) {
    if (ma.find(k) != ma.end()) {
      continue;
    }
    CompareRow r;
    r.scope = cb->scope;
    r.fsRwIndex = cb->fsRwIndex;
    r.path = cb->path;
    r.inA = false;
    r.inB = true;
    r.same = false;
    r.note = "B-only";
    rows[k] = r;
  }
  std::vector<CompareRow> out;
  for (auto& [k, r] : rows) {
    (void)k;
    out.push_back(r);
  }
  return out;
}

std::string renderCompare(const std::vector<CompareRow>& rows) {
  std::string s;
  size_t nA = 0, nB = 0, nSame = 0, nDiffer = 0;
  for (const CompareRow& r : rows) {
    s += scopeLabel(r.scope, r.fsRwIndex) + " " + r.path + ": " + r.note + "\n";
    if (r.inA && !r.inB) {
      ++nA;
    } else if (r.inB && !r.inA) {
      ++nB;
    } else if (r.same) {
      ++nSame;
    } else {
      ++nDiffer;
    }
  }
  s += std::to_string(rows.size()) + " path(s): " + std::to_string(nA) + " A-only, " +
       std::to_string(nB) + " B-only, " + std::to_string(nSame) + " same, " +
       std::to_string(nDiffer) + " differ\n";
  return s;
}

std::string renderCompareJson(const std::vector<CompareRow>& rows) {
  using snowglobe::util::jsonEscape;
  std::string s = "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    const CompareRow& r = rows[i];
    if (i > 0) {
      s += ",";
    }
    s += "{\"scope\":" + jsonEscape(scopeLabel(r.scope, r.fsRwIndex)) +
         ",\"path\":" + jsonEscape(r.path) + ",\"inA\":" + (r.inA ? "true" : "false") +
         ",\"inB\":" + (r.inB ? "true" : "false") + ",\"same\":" + (r.same ? "true" : "false") +
         ",\"note\":" + jsonEscape(r.note) + "}";
  }
  s += "]";
  return s;
}

bool eventPathReport(const std::string& eventsPath, std::string& out, std::string& error) {
  using snowglobe::link::findField;
  using snowglobe::link::readField;
  std::vector<uint8_t> bytes;
  if (!readWholeFile(eventsPath, bytes, error)) {
    error = "diff: " + error;
    return false;
  }
  // Per-path flags from fs events (ok:false failures ignored: no effect).
  std::map<std::string, unsigned> flags; // bit0 created, bit1 modified, bit2 deleted, bit3 renamed
  const std::string body(bytes.begin(), bytes.end());
  size_t start = 0;
  for (size_t i = 0; i <= body.size(); ++i) {
    if (i != body.size() && body[i] != '\n') {
      continue;
    }
    const std::string line = body.substr(start, i - start);
    start = i + 1;
    if (line.empty() || line[0] != '{') {
      continue;
    }
    std::string ev;
    {
      size_t vv = 0;
      if (!findField(line, 0, "ev", vv)) {
        continue;
      }
      const auto f = readField(line, vv);
      if (!f.found || !f.isString) {
        continue;
      }
      ev = f.str;
    }
    if (ev == "fs.open") {
      std::string p;
      size_t pv = 0;
      if (!findField(line, 0, "path", pv)) {
        continue;
      }
      {
        const auto f = readField(line, pv);
        if (!f.found || !f.isString) {
          continue;
        }
        p = f.str;
      }
      bool ok = true;
      {
        size_t ov = 0;
        if (findField(line, 0, "ok", ov)) {
          const auto f = readField(line, ov);
          ok = !f.found || !f.isBool || f.boolean;
        }
      }
      if (!ok) {
        continue;
      }
      bool write = false, create = false;
      {
        size_t wv = 0, cv = 0;
        if (findField(line, 0, "write", wv)) {
          const auto f = readField(line, wv);
          write = f.found && f.isBool && f.boolean;
        }
        if (findField(line, 0, "create", cv)) {
          const auto f = readField(line, cv);
          create = f.found && f.isBool && f.boolean;
        }
      }
      if (!write) {
        continue; // reads are not changes
      }
      flags[p] |= create ? 1u : 2u;
    } else if (ev == "fs.unlink" || ev == "fs.rmdir") {
      size_t pv = 0;
      if (!findField(line, 0, "path", pv)) {
        continue;
      }
      const auto f = readField(line, pv);
      if (!f.found || !f.isString) {
        continue;
      }
      size_t ov = 0;
      bool ok = true;
      if (findField(line, 0, "ok", ov)) {
        const auto g = readField(line, ov);
        ok = !g.found || !g.isBool || g.boolean;
      }
      if (ok) {
        flags[f.str] |= 4u;
      }
    } else if (ev == "fs.rename") {
      size_t fv = 0, tv = 0;
      std::string from, to;
      bool ok = true;
      {
        size_t ov = 0;
        if (findField(line, 0, "ok", ov)) {
          const auto g = readField(line, ov);
          ok = !g.found || !g.isBool || g.boolean;
        }
      }
      if (!ok) {
        continue;
      }
      if (findField(line, 0, "from", fv)) {
        const auto f = readField(line, fv);
        if (f.found && f.isString) {
          from = f.str;
        }
      }
      if (findField(line, 0, "to", tv)) {
        const auto f = readField(line, tv);
        if (f.found && f.isString) {
          to = f.str;
        }
      }
      if (!from.empty()) {
        flags[from] |= 8u;
      }
      if (!to.empty()) {
        flags[to] |= 8u;
      }
    } else if (ev == "fs.mkdir" || ev == "fs.symlink" || ev == "fs.chmod") {
      size_t pv = 0;
      if (!findField(line, 0, "path", pv)) {
        continue;
      }
      const auto f = readField(line, pv);
      if (!f.found || !f.isString) {
        continue;
      }
      size_t ov = 0;
      bool ok = true;
      if (findField(line, 0, "ok", ov)) {
        const auto g = readField(line, ov);
        ok = !g.found || !g.isBool || g.boolean;
      }
      if (ok) {
        flags[f.str] |= (ev == "fs.chmod") ? 2u : 1u;
      }
    }
  }
  out = "content not captured (run with --isolate for content)\n";
  for (const auto& [p, f] : flags) {
    std::string kind = "modified?";
    if (f & 4u) {
      kind = "deleted?";
    } else if (f & 8u) {
      kind = "renamed?";
    } else if (f & 1u) {
      kind = "created?";
    }
    out += kind + std::string(" ") + p + "\n";
  }
  out += std::to_string(flags.size()) + " path(s) from trace events (no content)\n";
  return true;
}

} // namespace snowglobe::diff

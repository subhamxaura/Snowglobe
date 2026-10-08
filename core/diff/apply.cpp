// Apply engine: validate-all-then-write (ADR-0008).
#include "apply.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../util/sha256.hpp"

namespace snowglobe::diff {
namespace {

std::string errnoText(int e) {
  const char* m = ::strerror(e);
  return m != nullptr ? std::string(m) : ("errno " + std::to_string(e));
}

std::string shaOf(const std::vector<uint8_t>& bytes) {
  return snowglobe::util::sha256Hex(bytes);
}

// Path safety (reported as rejections, never followed): reject absolute,
// empty, ".." components, and anything under .git/ (repo metadata is
// reviewed, never written by apply).
bool pathRejected(const std::string& rel, std::string& why) {
  if (rel.empty()) {
    why = "empty path";
    return true;
  }
  if (rel[0] == '/') {
    why = "absolute path";
    return true;
  }
  if (rel == ".git" || (rel.size() > 5 && rel.compare(0, 5, ".git/") == 0)) {
    why = "under .git/ (repo metadata is reviewed, never written)";
    return true;
  }
  size_t i = 0;
  while (i <= rel.size()) {
    size_t j = rel.find('/', i);
    if (j == std::string::npos) {
      j = rel.size();
    }
    const std::string comp = rel.substr(i, j - i);
    if (comp.empty() || comp == "." || comp == "..") {
      why = "unsafe component '" + comp + "'";
      return true;
    }
    i = j + 1;
  }
  return false;
}

// Open the parent chain of rel under rootFd with O_NOFOLLOW throughout
// (a symlink anywhere in the traversed components rejects the path).
// Returns the parent fd (close it) or -1 + why.
int openParentNoFollow(int rootFd, const std::string& rel, std::string& why) {
  const size_t slash = rel.rfind('/');
  int cur = ::dup(rootFd);
  if (cur < 0) {
    why = "dup project root: " + errnoText(errno);
    return -1;
  }
  if (slash == std::string::npos) {
    return cur; // parent is the root itself
  }
  size_t i = 0;
  while (i < slash) {
    size_t j = rel.find('/', i);
    const std::string comp = rel.substr(i, j - i);
    int next = ::openat(cur, comp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      why = "open component '" + comp + "': " + errnoText(errno);
      ::close(cur);
      return -1;
    }
    ::close(cur);
    cur = next;
    i = j + 1;
  }
  return cur;
}

std::string finalName(const std::string& rel) {
  const size_t slash = rel.rfind('/');
  return (slash == std::string::npos) ? rel : rel.substr(slash + 1);
}

// fstatat-or-missing for validation (D1): present, ENOENT-missing, or a
// hard error. Callers must abort validation (70, zero writes) on error —
// treating EACCES as absent would stage writes that fail mid-apply and
// break all-or-nothing.
bool statFinal(int rootFd, const std::string& rel, struct stat& st, bool& missing,
               std::string& error) {
  if (::fstatat(rootFd, rel.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
    missing = false;
    return true;
  }
  if (errno == ENOENT) {
    missing = true;
    return true;
  }
  error = "stat " + rel + ": " + errnoText(errno);
  return false;
}

// Recursive rm -rf under parentFd/name with O_NOFOLLOW everywhere:
// symlinks are unlinked, never descended.
bool removeTreeAt(int parentFd, const std::string& name, std::string& error) {
  struct stat st = {};
  if (::fstatat(parentFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    error = "stat " + name + ": " + errnoText(errno);
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    if (::unlinkat(parentFd, name.c_str(), 0) != 0 && errno != ENOENT) {
      error = "unlink " + name + ": " + errnoText(errno);
      return false;
    }
    return true;
  }
  int dirFd = ::openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirFd < 0) {
    error = "open dir " + name + ": " + errnoText(errno);
    return false;
  }
  DIR* d = ::fdopendir(dirFd);
  if (d == nullptr) {
    error = "fdopendir " + name + ": " + errnoText(errno);
    ::close(dirFd);
    return false;
  }
  std::vector<std::string> names;
  for (;;) {
    errno = 0;
    struct dirent* de = ::readdir(d);
    if (de == nullptr) {
      if (errno != 0) {
        error = "readdir " + name + ": " + errnoText(errno);
        ::closedir(d);
        return false;
      }
      break;
    }
    const std::string n = de->d_name;
    if (n != "." && n != "..") {
      names.push_back(n);
    }
  }
  ::closedir(d); // closes dirFd too
  dirFd = ::openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirFd < 0) {
    if (errno == ENOENT) {
      return true; // raced away; goal state holds
    }
    error = "reopen dir " + name + ": " + errnoText(errno);
    return false;
  }
  for (const std::string& n : names) {
    if (!removeTreeAt(dirFd, n, error)) {
      ::close(dirFd);
      return false;
    }
  }
  ::close(dirFd);
  if (::unlinkat(parentFd, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
    error = "rmdir " + name + ": " + errnoText(errno);
    return false;
  }
  return true;
}

// Exact subtree match for directory deletes (D1): 0 = host matches the
// baseline exactly (regulars: sha; links: target; dirs recurse; extras on
// either side are mismatches except baseline entries already gone from
// the host, which is consistent with deletion), 1 = mismatch, -1 + error
// on IO failure. Symlinks never followed. An extra host file would
// otherwise be deleted silently by the rmtree — hence the strictness.
int checkDirFd(int dirFd, const std::string& rel, const Baseline& baseline, std::string& error) {
  DIR* d = ::fdopendir(dirFd); // takes ownership of dirFd
  if (d == nullptr) {
    error = "fdopendir " + rel + ": " + errnoText(errno);
    ::close(dirFd);
    return -1;
  }
  int rc = 0;
  for (;;) {
    errno = 0;
    struct dirent* de = ::readdir(d);
    if (de == nullptr) {
      if (errno != 0) {
        error = "readdir " + rel + ": " + errnoText(errno);
        rc = -1;
      }
      break;
    }
    const std::string n = de->d_name;
    if (n == "." || n == "..") {
      continue;
    }
    const std::string sub = rel + "/" + n;
    struct stat st = {};
    if (::fstatat(dirFd, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        continue; // raced away: goal state holds
      }
      error = "stat " + sub + ": " + errnoText(errno);
      rc = -1;
      break;
    }
    const auto bit = baseline.files.find(sub);
    if (S_ISDIR(st.st_mode)) {
      // A host dir matches only with baseline entries beneath it.
      auto it = baseline.files.lower_bound(sub + "/");
      if (it == baseline.files.end() ||
          it->first.compare(0, sub.size() + 1, sub + "/") != 0) {
        rc = 1; // extra dir: would be deleted silently
        break;
      }
      const int subFd =
          ::openat(dirFd, n.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (subFd < 0) {
        error = "open dir " + sub + ": " + errnoText(errno);
        rc = -1;
        break;
      }
      rc = checkDirFd(subFd, sub, baseline, error); // takes subFd
      if (rc != 0) {
        break;
      }
      continue;
    }
    if (bit == baseline.files.end()) {
      rc = 1; // extra file/link/special: would be deleted silently
      break;
    }
    if (S_ISREG(st.st_mode) && !bit->second.isLink) {
      const int fd = ::openat(dirFd, n.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        error = "open " + sub + ": " + errnoText(errno);
        rc = -1;
        break;
      }
      std::vector<uint8_t> host;
      char buf[65536];
      bool rdOk = true;
      for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r < 0) {
          if (errno == EINTR) {
            continue;
          }
          rdOk = false;
          break;
        }
        if (r == 0) {
          break;
        }
        host.insert(host.end(), buf, buf + r);
      }
      ::close(fd);
      if (!rdOk || shaOf(host) != bit->second.shaHex) {
        rc = 1;
        break;
      }
    } else if (S_ISLNK(st.st_mode) && bit->second.isLink) {
      char tgt[4096];
      const ssize_t len = ::readlinkat(dirFd, n.c_str(), tgt, sizeof(tgt) - 1);
      if (len < 0 || std::string(tgt, static_cast<size_t>(len)) != bit->second.target) {
        rc = 1;
        break;
      }
    } else {
      rc = 1; // type changed, or special where baseline has content
      break;
    }
  }
  ::closedir(d); // closes dirFd
  return rc;
}

bool mkdirParents(int rootFd, const std::string& rel, std::string& error) {
  const size_t slash = rel.rfind('/');
  if (slash == std::string::npos) {
    return true;
  }
  size_t i = 0;
  int cur = ::dup(rootFd);
  if (cur < 0) {
    error = "dup project root: " + errnoText(errno);
    return false;
  }
  bool ok = true;
  while (i < slash) {
    size_t j = rel.find('/', i);
    const std::string comp = rel.substr(i, j - i);
    if (::mkdirat(cur, comp.c_str(), 0755) != 0 && errno != EEXIST) {
      error = "mkdir " + comp + ": " + errnoText(errno);
      ok = false;
      break;
    }
    int next = ::openat(cur, comp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      error = "open component '" + comp + "': " + errnoText(errno);
      ok = false;
      break;
    }
    ::close(cur);
    cur = next;
    i = j + 1;
  }
  // fsync the chain tail for durability (best-effort on the intermediates).
  if (ok) {
    (void)::fsync(cur);
  }
  ::close(cur);
  return ok;
}

} // namespace

std::string renderApplyReport(const ApplyPlan& plan, bool dryRun) {
  std::string s = dryRun ? "apply --dry-run (no writes)\n" : "apply report\n";
  for (const ApplyOp& op : plan.ops) {
    const char* what = (op.action == 'W')   ? "write"
                       : (op.action == 'L') ? "symlink"
                       : (op.action == 'U') ? "unlink"
                                            : "rmtree";
    s += std::string("  ") + what + " " + op.path;
    if (op.action == 'L') {
      s += " -> " + op.linkTarget;
    }
    s += "\n";
  }
  for (const std::string& p : plan.alreadyApplied) {
    s += std::string("  skip (already applied) ") + p + "\n";
  }
  for (const std::string& c : plan.conflicts) {
    s += std::string("  CONFLICT ") + c + "\n";
  }
  for (const std::string& r : plan.rejected) {
    s += std::string("  REJECTED ") + r + "\n";
  }
  if (plan.clean()) {
    s += std::to_string(plan.ops.size()) + " op(s), " + std::to_string(plan.alreadyApplied.size()) +
         " already applied\n";
  } else {
    s += "aborted: " + std::to_string(plan.conflicts.size()) + " conflict(s), " +
         std::to_string(plan.rejected.size()) + " rejected; NOTHING written\n";
  }
  return s;
}

bool planApply(const std::string& projectAbs, const std::string& runDir, ApplyPlan& plan,
               std::string& error) {
  plan = ApplyPlan();
  // Baseline + uppers must exist (69-class handled by the caller via
  // computeChanges errors? No: distinguish here for the exit code).
  Baseline baseline;
  if (!loadBaseline(runDir + "/baseline.json", baseline, error)) {
    return false; // caller maps missing->69, corrupt->70 by message? see below
  }
  const UpperRoots uppers = findUppers(runDir);
  if (uppers.project.empty()) {
    error = "apply: no project upper in " + runDir;
    return false;
  }
  ChangeSet cs;
  // Fresh recompute (host state may have moved since any earlier diff).
  if (!computeChanges(runDir, projectAbs, baseline, false, cs, error)) {
    return false;
  }
  const int rootFd = ::open(projectAbs.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (rootFd < 0) {
    error = "apply: open project " + projectAbs + ": " + errnoText(errno);
    return false;
  }
  // Validate every project change; system scopes are never applied.
  for (const Change& c : cs.changes) {
    if (c.scope != Scope::Project) {
      continue;
    }
    std::string why;
    if (pathRejected(c.path, why) || (c.kind == 'R' && pathRejected(c.newPath, why))) {
      plan.rejected.push_back(c.path + ": " + why);
      continue;
    }
    // Parent chain resolvability (proves no traversed symlink now; the
    // write phase re-resolves, so a mid-apply race still fails loudly).
    const std::string& probePath = (c.kind == 'R') ? c.newPath : c.path;
    std::string pw;
    const int pfd = openParentNoFollow(rootFd, probePath, pw);
    if (pfd < 0) {
      // Missing parents are created at write time — but a symlink in the
      // chain must reject NOW (fail closed on what we can see).
      if (pw.find("open component") != std::string::npos) {
        // Distinguish absent dir (ok, will mkdir) from symlink (reject):
        // re-walk to find the first failing component.
        size_t i = 0;
        const size_t slash = probePath.rfind('/');
        bool symlinkFound = false;
        int cur = ::dup(rootFd);
        if (cur >= 0) {
          while (i < slash) {
            size_t j = probePath.find('/', i);
            const std::string comp = probePath.substr(i, j - i);
            struct stat st = {};
            if (::fstatat(cur, comp.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
              break; // absent from here on: will be created
            }
            if (!S_ISDIR(st.st_mode)) {
              if (S_ISLNK(st.st_mode)) {
                symlinkFound = true;
              }
              break;
            }
            int next = ::openat(cur, comp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) {
              if (errno == ELOOP) {
                symlinkFound = true;
              }
              break;
            }
            ::close(cur);
            cur = next;
            i = j + 1;
          }
          if (cur >= 0) {
            ::close(cur);
          }
        }
        if (symlinkFound) {
          plan.rejected.push_back(c.path + ": symlink in parent chain");
          continue;
        }
        // Absent parents: fall through to content validation (write phase
        // creates them with the same NOFOLLOW discipline).
      } else {
        plan.rejected.push_back(c.path + ": " + pw);
        continue;
      }
    } else {
      ::close(pfd);
    }
    // Final-component state + content validation. Symlinks in *parent*
    // components were rejected above; a symlink in final position is
    // validated per kind below (writes never follow: temp+rename replaces
    // a raced-in link instead of traversing it, and unlink removes the
    // link itself).
    struct stat fst = {};
    bool missing = false;
    if (!statFinal(rootFd, probePath, fst, missing, error)) {
      ::close(rootFd);
      error = "apply: " + error;
      return false;
    }
    const bool exists = !missing;
    if (c.kind == 'A') {
      if (c.isLink) {
        if (exists) {
          plan.conflicts.push_back(c.path + ": host exists (link add)");
        } else {
          ApplyOp op;
          op.path = c.path;
          op.action = 'L';
          op.linkTarget = c.linkTarget;
          plan.ops.push_back(std::move(op));
        }
        continue;
      }
      if (exists) {
        plan.conflicts.push_back(c.path + ": host exists");
        continue;
      }
      std::vector<uint8_t> bytes;
      unsigned mode = 0644;
      if (!readUpperBytes(uppers.project, c.path, bytes, mode, error)) {
        ::close(rootFd);
        error = "apply: " + error;
        return false;
      }
      ApplyOp op;
      op.path = c.path;
      op.action = 'W';
      op.bytes = std::move(bytes);
      op.mode = mode;
      plan.ops.push_back(std::move(op));
    } else if (c.kind == 'M') {
      // Host must still hold the baseline content (or the baseline link).
      bool okBase = false;
      if (exists && S_ISREG(fst.st_mode) && !c.oldSha.empty()) {
        std::vector<uint8_t> host;
        // Read via rootFd-relative open (no follow: a raced-in symlink
        // fails ELOOP here and lands in the conflict below, never read).
        const int fd = ::openat(rootFd, probePath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
          host.clear();
          char buf[65536];
          bool rdOk = true;
          for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
              if (errno == EINTR) {
                continue;
              }
              rdOk = false;
              break;
            }
            if (n == 0) {
              break;
            }
            host.insert(host.end(), buf, buf + n);
          }
          ::close(fd);
          if (rdOk && shaOf(host) == c.oldSha) {
            okBase = true;
          }
        }
      } else if (exists && S_ISLNK(fst.st_mode)) {
        okBase = false; // symlink where a file is expected: host moved on
      } else if (!exists) {
        plan.conflicts.push_back(c.path + ": host missing (modified in run)");
        continue;
      }
      if (!okBase) {
        // Baseline-link M (oldSha ""): host must still be that link.
        if (c.oldSha.empty()) {
          const auto bit = baseline.files.find(c.path);
          if (bit != baseline.files.end() && bit->second.isLink) {
            // Re-stat through the parent (validated): compare target.
            std::string pw2;
            const int pfd2 = openParentNoFollow(rootFd, probePath, pw2);
            if (pfd2 >= 0) {
              char t2[4096];
              const ssize_t l2 =
                  ::readlinkat(pfd2, finalName(probePath).c_str(), t2, sizeof(t2) - 1);
              ::close(pfd2);
              if (l2 >= 0 && std::string(t2, static_cast<size_t>(l2)) == bit->second.target) {
                okBase = true;
              }
            }
          }
        }
      }
      if (!okBase) {
        plan.conflicts.push_back(c.path + ": host differs from baseline");
        continue;
      }
      if (c.isLink) {
        ApplyOp op;
        op.path = c.path;
        op.action = 'L';
        op.linkTarget = c.linkTarget;
        // Write phase unlinks first (link->link replace is unlink+symlink).
        plan.ops.push_back(std::move(op));
      } else {
        std::vector<uint8_t> bytes;
        unsigned mode = 0644;
        if (!readUpperBytes(uppers.project, c.path, bytes, mode, error)) {
          ::close(rootFd);
          error = "apply: " + error;
          return false;
        }
        ApplyOp op;
        op.path = c.path;
        op.action = 'W';
        op.bytes = std::move(bytes);
        op.mode = mode;
        plan.ops.push_back(std::move(op));
      }
    } else if (c.kind == 'D') {
      if (!exists) {
        plan.alreadyApplied.push_back(c.path);
        continue;
      }
      if (!S_ISREG(fst.st_mode) && !S_ISDIR(fst.st_mode) && !S_ISLNK(fst.st_mode)) {
        plan.conflicts.push_back(c.path + ": host is a special file");
        continue;
      }
      if (S_ISLNK(fst.st_mode)) {
        // Unlinking a symlink removes the link itself (never the target):
        // safe, and the agent-visible end state (gone) is achieved.
        ApplyOp op;
        op.path = c.path;
        op.action = 'U';
        plan.ops.push_back(std::move(op));
        continue;
      }
      if (S_ISDIR(fst.st_mode)) {
        // Directory delete: the host subtree must match the baseline
        // exactly (extras included — they would otherwise vanish silently).
        const int dirFd =
            ::openat(rootFd, probePath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dirFd < 0) {
          plan.conflicts.push_back(c.path + ": cannot open host dir");
          continue;
        }
        const int m = checkDirFd(dirFd, c.path, baseline, error);
        if (m < 0) {
          ::close(rootFd);
          error = "apply: " + error;
          return false;
        }
        if (m > 0) {
          plan.conflicts.push_back(c.path + ": subtree differs from baseline");
          continue;
        }
        ApplyOp op;
        op.path = c.path;
        op.action = 'X';
        plan.ops.push_back(std::move(op));
      } else {
        // Regular file delete: content must match baseline (when known).
        if (!c.oldSha.empty()) {
          const int fd = ::openat(rootFd, probePath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
          if (fd < 0) {
            plan.conflicts.push_back(c.path + ": cannot read host file");
            continue;
          }
          std::vector<uint8_t> host;
          char buf[65536];
          bool rdOk = true;
          for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
              if (errno == EINTR) {
                continue;
              }
              rdOk = false;
              break;
            }
            if (n == 0) {
              break;
            }
            host.insert(host.end(), buf, buf + n);
          }
          ::close(fd);
          if (!rdOk || shaOf(host) != c.oldSha) {
            plan.conflicts.push_back(c.path + ": host differs from baseline");
            continue;
          }
        }
        ApplyOp op;
        op.path = c.path;
        op.action = 'U';
        plan.ops.push_back(std::move(op));
      }
    } else if (c.kind == 'R') {
      // Rename as delete-old + create-new, each validated like D and A.
      // Old side:
      bool oldGone = false;
      {
        struct stat ost = {};
        bool omissing = false;
        if (!statFinal(rootFd, c.path, ost, omissing, error)) {
          ::close(rootFd);
          error = "apply: " + error;
          return false;
        }
        const bool oexists = !omissing;
        if (!oexists) {
          oldGone = true;
        } else if (S_ISLNK(ost.st_mode)) {
          plan.rejected.push_back(c.path + ": final component is a symlink (refusing to follow)");
          continue;
        } else if (S_ISREG(ost.st_mode) && !c.oldSha.empty()) {
          const int fd = ::openat(rootFd, c.path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
          if (fd < 0) {
            plan.conflicts.push_back(c.path + ": cannot read host file");
            continue;
          }
          std::vector<uint8_t> host;
          char buf[65536];
          bool rdOk = true;
          for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
              if (errno == EINTR) {
                continue;
              }
              rdOk = false;
              break;
            }
            if (n == 0) {
              break;
            }
            host.insert(host.end(), buf, buf + n);
          }
          ::close(fd);
          if (!rdOk || shaOf(host) != c.oldSha) {
            plan.conflicts.push_back(c.path + ": host differs from baseline");
            continue;
          }
        } else {
          plan.conflicts.push_back(c.path + ": unexpected host type");
          continue;
        }
      }
      // New side:
      {
        struct stat nst = {};
        bool nmissing = false;
        if (!statFinal(rootFd, c.newPath, nst, nmissing, error)) {
          ::close(rootFd);
          error = "apply: " + error;
          return false;
        }
        if (!nmissing) {
          plan.conflicts.push_back(c.newPath + ": host exists");
          continue;
        }
      }
      if (!oldGone) {
        ApplyOp op;
        op.path = c.path;
        op.action = 'U';
        plan.ops.push_back(std::move(op));
      }
      std::vector<uint8_t> bytes;
      unsigned mode = 0644;
      if (!readUpperBytes(uppers.project, c.newPath, bytes, mode, error)) {
        ::close(rootFd);
        error = "apply: " + error;
        return false;
      }
      ApplyOp op;
      op.path = c.newPath;
      op.action = 'W';
      op.bytes = std::move(bytes);
      op.mode = mode;
      plan.ops.push_back(std::move(op));
    } else if (c.kind == 'S') {
      plan.rejected.push_back(c.path + ": special file (never applied)");
    }
  }
  ::close(rootFd);
  // Sort ops by path for a stable report (deletes before writes at the
  // same... no cross-path ordering constraint: validation already proved
  // independence; path order is deterministic).
  std::sort(plan.ops.begin(), plan.ops.end(),
            [](const ApplyOp& x, const ApplyOp& y) { return x.path < y.path; });
  return true;
}

bool execApply(const std::string& projectAbs, const ApplyPlan& plan, std::string& error) {
  const int rootFd = ::open(projectAbs.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (rootFd < 0) {
    error = "apply: open project " + projectAbs + ": " + errnoText(errno);
    return false;
  }
  for (const ApplyOp& op : plan.ops) {
    if (op.action == 'U' || op.action == 'X') {
      // Re-resolve the parent (write phase repeats the NOFOLLOW walk so a
      // mid-apply race still fails loudly instead of following).
      std::string pw;
      const int pfd = openParentNoFollow(rootFd, op.path, pw);
      if (pfd < 0) {
        error = "apply: " + pw + " for " + op.path;
        ::close(rootFd);
        return false;
      }
      const std::string fin = finalName(op.path);
      bool ok = false;
      if (op.action == 'U') {
        struct stat st = {};
        if (::fstatat(pfd, fin.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
          ok = (errno == ENOENT);
          if (!ok) {
            error = "apply: stat " + op.path + ": " + errnoText(errno);
          }
        } else if (S_ISDIR(st.st_mode)) {
          error = "apply: " + op.path + " became a directory mid-apply (aborting)";
        } else if (::unlinkat(pfd, fin.c_str(), 0) == 0 || errno == ENOENT) {
          // Regular files and symlinks alike: unlink removes the entry
          // itself (a link is never followed). Planned as such in phase 1.
          ok = true;
        } else {
          error = "apply: unlink " + op.path + ": " + errnoText(errno);
        }
      } else {
        ok = removeTreeAt(pfd, fin, error);
      }
      ::close(pfd);
      if (!ok) {
        ::close(rootFd);
        return false;
      }
      continue;
    }
    if (op.action == 'L') {
      // Unlink-then-symlink (covers link->link replace; fresh adds skip
      // the unlink when absent).
      if (!mkdirParents(rootFd, op.path, error)) {
        error = "apply: " + error;
        ::close(rootFd);
        return false;
      }
      std::string pw;
      const int pfd = openParentNoFollow(rootFd, op.path, pw);
      if (pfd < 0) {
        error = "apply: " + pw + " for " + op.path;
        ::close(rootFd);
        return false;
      }
      const std::string fin = finalName(op.path);
      (void)::unlinkat(pfd, fin.c_str(), 0);
      if (::symlinkat(op.linkTarget.c_str(), pfd, fin.c_str()) != 0) {
        error = "apply: symlink " + op.path + ": " + errnoText(errno);
        ::close(pfd);
        ::close(rootFd);
        return false;
      }
      (void)::fsync(pfd);
      ::close(pfd);
      continue;
    }
    if (op.action == 'W') {
      if (!mkdirParents(rootFd, op.path, error)) {
        error = "apply: " + error;
        ::close(rootFd);
        return false;
      }
      std::string pw;
      const int pfd = openParentNoFollow(rootFd, op.path, pw);
      if (pfd < 0) {
        error = "apply: " + pw + " for " + op.path;
        ::close(rootFd);
        return false;
      }
      // Temp + rename (never truncate-in-place): a mid-write failure
      // (ENOSPC, crash) leaves the old file intact; renameat over a
      // raced-in symlink replaces the link itself (never follows).
      const std::string fin = finalName(op.path);
      const std::string tmp =
          fin + ".sg-apply-" + std::to_string(static_cast<long long>(::getpid())) + ".tmp";
      (void)::unlinkat(pfd, tmp.c_str(), 0); // stale tmp from a dead apply
      int fd =
          ::openat(pfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0) {
        error = "apply: create tmp for " + op.path + ": " + errnoText(errno);
        ::close(pfd);
        ::close(rootFd);
        return false;
      }
      size_t done = 0;
      bool wok = true;
      while (done < op.bytes.size()) {
        const ssize_t n = ::write(fd, op.bytes.data() + done, op.bytes.size() - done);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          error = "apply: write " + op.path + ": " + errnoText(errno);
          wok = false;
          break;
        }
        done += static_cast<size_t>(n);
      }
      if (wok && ::fchmod(fd, op.mode & 0777) != 0) {
        error = "apply: chmod " + op.path + ": " + errnoText(errno);
        wok = false;
      }
      if (wok && ::fsync(fd) != 0) {
        error = "apply: fsync " + op.path + ": " + errnoText(errno);
        wok = false;
      }
      ::close(fd);
      if (wok && ::renameat(pfd, tmp.c_str(), pfd, fin.c_str()) != 0) {
        error = "apply: rename " + op.path + ": " + errnoText(errno);
        wok = false;
      }
      if (!wok) {
        (void)::unlinkat(pfd, tmp.c_str(), 0);
      }
      (void)::fsync(pfd);
      ::close(pfd);
      if (!wok) {
        ::close(rootFd);
        return false;
      }
      continue;
    }
    error = "apply: unknown op '" + std::string(1, op.action) + "' for " + op.path;
    ::close(rootFd);
    return false;
  }
  ::close(rootFd);
  return true;
}

} // namespace snowglobe::diff

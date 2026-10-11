// Project baseline walk + strict reader (ADR-0008).
#include "baseline.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../link/event_scan.hpp"
#include "../proxy/model_scan.hpp"
#include "../util/sha256.hpp"
#include "../util/string_util.hpp"

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

// Phase 1: metadata-only recursive walk with lstat (symlinks never
// followed, so no escape and no cycles). Regular files are collected for
// phase 2; symlinks are recorded inline (readlink is cheap); anything else
// counts as skippedSpecial. Children are visited in sorted byte order so
// the collected list is deterministic. rel uses '/' and is "" for the
// root itself (never an entry).
struct PendingFile {
  std::string rel;
  std::string abs;
  int64_t mtime = 0; // st_mtime, forensics only (never a diff/apply key)
};

bool collectDir(const std::string& abs, const std::string& rel,
                const std::vector<std::string>& excludes, Baseline& out,
                std::vector<PendingFile>& pending, std::string& error) {
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
      continue;
    }
    const std::string childAbs = abs + "/" + n;
    struct stat st = {};
    if (::lstat(childAbs.c_str(), &st) != 0) {
      error = "lstat " + childAbs + ": " + errnoText(errno);
      return false;
    }
    if (S_ISLNK(st.st_mode)) {
      char tgt[4096];
      const ssize_t len = ::readlink(childAbs.c_str(), tgt, sizeof(tgt) - 1);
      if (len < 0) {
        error = "readlink " + childAbs + ": " + errnoText(errno);
        return false;
      }
      BaselineEntry e;
      e.isLink = true;
      e.target = std::string(tgt, static_cast<size_t>(len));
      e.mtime = static_cast<int64_t>(st.st_mtime);
      out.files[childRel] = e;
    } else if (S_ISREG(st.st_mode)) {
      PendingFile p;
      p.rel = childRel;
      p.abs = childAbs;
      p.mtime = static_cast<int64_t>(st.st_mtime);
      pending.push_back(std::move(p));
    } else if (S_ISDIR(st.st_mode)) {
      if (!collectDir(childAbs, childRel, excludes, out, pending, error)) {
        return false;
      }
    } else {
      ++out.skippedSpecial; // fifo/socket/device: documented skip, counted
    }
  }
  return true;
}

bool writeAll(FILE* f, const std::string& s) {
  return s.empty() || ::fwrite(s.data(), 1, s.size(), f) == s.size();
}

// Phase 2: hash the collected regular files over a thread pool (issue #5).
// Same portable SHA-256 as before (no dep change, baseline semantics stay
// sha256); sharding is by atomic work-stealing index, so output is
// independent of scheduling and the merged map is deterministic. Any
// unreadable file aborts loudly (false + error, never a silent hole).
// Walks slower than 2 s print a progress line on stderr (the caller in
// main.cpp prints the final files/ms note either way).
bool hashPending(const std::vector<PendingFile>& pending,
                 const std::chrono::steady_clock::time_point& t0, Baseline& out,
                 std::string& error) {
  struct Hashed {
    std::string shaHex;
    uint64_t size = 0;
  };
  std::vector<Hashed> results(pending.size());
  if (pending.empty()) {
    return true;
  }
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) {
    hc = 4;
  }
  size_t nThreads = std::min<size_t>(hc, pending.size());
  std::atomic<size_t> next{0};
  std::atomic<size_t> done{0};
  std::atomic<bool> failed{false};
  std::mutex errMu;
  std::string firstError;
  std::vector<std::thread> workers;
  workers.reserve(nThreads);
  for (size_t t = 0; t < nThreads; ++t) {
    workers.emplace_back([&]() {
      std::vector<uint8_t> bytes; // per-thread scratch (one file at a time)
      for (;;) {
        if (failed.load(std::memory_order_relaxed)) {
          return;
        }
        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= pending.size()) {
          return;
        }
        std::string localErr;
        if (!readWholeFile(pending[i].abs, bytes, localErr)) {
          std::lock_guard<std::mutex> lk(errMu);
          if (firstError.empty()) {
            firstError = localErr;
          }
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        results[i].shaHex = snowglobe::util::sha256Hex(bytes);
        results[i].size = static_cast<uint64_t>(bytes.size());
        done.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  // Progress watch while the pool works: silent under 2 s, then a line
  // per second on stderr until the pool drains.
  auto lastPrint = t0;
  for (;;) {
    if (done.load(std::memory_order_relaxed) >= pending.size() ||
        failed.load(std::memory_order_relaxed)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (now - t0 > std::chrono::seconds(2) && now - lastPrint >= std::chrono::seconds(1)) {
      lastPrint = now;
      std::fprintf(stderr, "note: baseline hashing %zu/%zu files...\n", done.load(),
                   pending.size());
    }
  }
  for (auto& w : workers) {
    w.join();
  }
  if (!firstError.empty()) {
    error = firstError;
    return false;
  }
  if (failed.load()) {
    error = "hash pool aborted";
    return false;
  }
  for (size_t i = 0; i < pending.size(); ++i) {
    BaselineEntry e;
    e.isLink = false;
    e.shaHex = results[i].shaHex;
    e.size = results[i].size;
    e.mtime = pending[i].mtime;
    out.files[pending[i].rel] = e;
  }
  return true;
}

} // namespace

bool isExcluded(const std::string& rel, const std::vector<std::string>& excludes) {
  for (const std::string& x : excludes) {
    if (x.empty()) {
      continue;
    }
    if (rel == x) {
      return true;
    }
    if (rel.size() > x.size() && rel.compare(0, x.size(), x) == 0 && rel[x.size()] == '/') {
      return true;
    }
  }
  return false;
}

bool writeBaseline(const std::string& projectAbs, const std::string& outPath,
                   const std::vector<std::string>& extraExcludes, Baseline& out,
                   std::string& error) {
  out = Baseline();
  out.project = projectAbs;
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> excludes = defaultBaselineExcludes();
  excludes.insert(excludes.end(), extraExcludes.begin(), extraExcludes.end());
  // Never hash our own output when it lands inside the walked tree.
  if (outPath.compare(0, projectAbs.size(), projectAbs) == 0 &&
      outPath.size() > projectAbs.size() && outPath[projectAbs.size()] == '/') {
    excludes.push_back(outPath.substr(projectAbs.size() + 1));
  }
  std::vector<PendingFile> pending;
  if (!collectDir(projectAbs, "", excludes, out, pending, error)) {
    error = "baseline: " + error;
    return false;
  }
  if (!hashPending(pending, t0, out, error)) {
    error = "baseline: " + error;
    return false;
  }
  using snowglobe::util::jsonEscape;
  out.excludes = excludes;
  std::string body = "{\"version\":1,\"project\":" + jsonEscape(projectAbs) + ",\"excludes\":[";
  for (size_t i = 0; i < excludes.size(); ++i) {
    if (i > 0) {
      body += ",";
    }
    body += jsonEscape(excludes[i]);
  }
  body += "],\"files\":{";
  bool first = true;
  for (const auto& [rel, e] : out.files) {
    if (!first) {
      body += ",";
    }
    first = false;
    body += jsonEscape(rel) + ":";
    if (e.isLink) {
      body += "{\"type\":\"link\",\"target\":" + jsonEscape(e.target) +
              ",\"mtime\":" + std::to_string(e.mtime) + "}";
    } else {
      body += "{\"type\":\"file\",\"sha256\":" + jsonEscape(e.shaHex) +
              ",\"size\":" + std::to_string(e.size) + ",\"mtime\":" + std::to_string(e.mtime) + "}";
    }
  }
  body += "},\"skipped_special\":" + std::to_string(out.skippedSpecial) + "}";
  // Write via a .tmp sibling + rename (a torn baseline.json is a corrupt
  // run artifact; crash mid-write must not leave a half file behind).
  const std::string tmp = outPath + ".tmp";
  FILE* f = ::fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    error = "baseline: open " + tmp + ": " + errnoText(errno);
    return false;
  }
  if (!writeAll(f, body) || ::fclose(f) != 0) {
    error = "baseline: write " + tmp + ": " + errnoText(errno);
    ::fclose(f);
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), outPath.c_str()) != 0) {
    error = "baseline: rename " + tmp + ": " + errnoText(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool loadBaseline(const std::string& path, Baseline& out, std::string& error) {
  using snowglobe::link::findField;
  using snowglobe::link::readField;
  using snowglobe::proxy::parseJsonString;
  using snowglobe::proxy::skipValue;
  using snowglobe::proxy::skipWs;
  out = Baseline();
  std::vector<uint8_t> bytes;
  if (!readWholeFile(path, bytes, error)) {
    error = "baseline: " + error;
    return false;
  }
  const std::string body(bytes.begin(), bytes.end());
  auto fail = [&](const std::string& why) {
    error = "baseline: corrupt " + path + ": " + why;
    return false;
  };
  size_t v = 0;
  if (!findField(body, 0, "version", v)) {
    return fail("missing version");
  }
  {
    const auto f = readField(body, v);
    if (!f.found || !f.isNumber || f.num != 1) {
      return fail("unsupported version (want 1)");
    }
  }
  if (!findField(body, 0, "project", v)) {
    return fail("missing project");
  }
  {
    const auto f = readField(body, v);
    if (!f.found || !f.isString) {
      return fail("bad project");
    }
    out.project = f.str;
  }
  {
    // Effective excludes (required in v1): diff replays the same walk, so
    // the classification reproduces exactly at review time.
    size_t xv = 0;
    if (!findField(body, 0, "excludes", xv)) {
      return fail("missing excludes");
    }
    const auto xf = readField(body, xv);
    if (!xf.found || !xf.isArray) {
      return fail("bad excludes");
    }
    size_t i = xv + 1;
    skipWs(body, i);
    if (i < body.size() && body[i] == ']') {
      i += 1;
    } else {
      for (;;) {
        skipWs(body, i);
        std::string x;
        const size_t e = parseJsonString(body, i, x);
        if (e == std::string::npos) {
          return fail("bad excludes entry");
        }
        out.excludes.push_back(x);
        i = e;
        skipWs(body, i);
        if (i < body.size() && body[i] == ',') {
          ++i;
          continue;
        }
        if (i < body.size() && body[i] == ']') {
          ++i;
          break;
        }
        return fail("bad excludes separator");
      }
    }
  }
  if (!findField(body, 0, "files", v)) {
    return fail("missing files");
  }
  if (v >= body.size() || body[v] != '{') {
    return fail("bad files object");
  }
  // Walk the files object member by member (strict: every member must be
  // a well-formed entry object).
  size_t i = v + 1;
  skipWs(body, i);
  if (i < body.size() && body[i] == '}') {
    i += 1; // empty files object
  } else {
    for (;;) {
      skipWs(body, i);
      std::string rel;
      size_t e = parseJsonString(body, i, rel);
      if (e == std::string::npos) {
        return fail("bad files key");
      }
      i = e;
      skipWs(body, i);
      if (i >= body.size() || body[i] != ':') {
        return fail("bad files separator");
      }
      ++i;
      skipWs(body, i);
      if (i >= body.size() || body[i] != '{') {
        return fail("bad entry object");
      }
      const size_t obj = i;
      size_t fv = 0;
      if (!findField(body, obj, "type", fv)) {
        return fail("entry missing type");
      }
      std::string type;
      {
        const auto f = readField(body, fv);
        if (!f.found || !f.isString) {
          return fail("bad entry type");
        }
        type = f.str;
      }
      BaselineEntry be;
      if (type == "file") {
        be.isLink = false;
        size_t sv = 0, zv = 0, mv = 0;
        if (!findField(body, obj, "sha256", sv) || !findField(body, obj, "size", zv) ||
            !findField(body, obj, "mtime", mv)) {
          return fail("file entry missing fields");
        }
        const auto fs = readField(body, sv);
        const auto fz = readField(body, zv);
        const auto fm = readField(body, mv);
        if (!fs.found || !fs.isString || fs.str.size() != 64 || !fz.found || !fz.isNumber ||
            fz.num < 0 || !fm.found || !fm.isNumber) {
          return fail("bad file entry fields");
        }
        be.shaHex = fs.str;
        be.size = static_cast<uint64_t>(fz.num);
        be.mtime = static_cast<int64_t>(fm.num);
      } else if (type == "link") {
        be.isLink = true;
        size_t tv = 0, mv = 0;
        if (!findField(body, obj, "target", tv) || !findField(body, obj, "mtime", mv)) {
          return fail("link entry missing fields");
        }
        const auto ft = readField(body, tv);
        const auto fm = readField(body, mv);
        if (!ft.found || !ft.isString || !fm.found || !fm.isNumber) {
          return fail("bad link entry fields");
        }
        be.target = ft.str;
        be.mtime = static_cast<int64_t>(fm.num);
      } else {
        return fail("unknown entry type");
      }
      if (rel.empty() || rel[0] == '/') {
        return fail("bad entry path");
      }
      out.files[rel] = be;
      i = obj;
      skipValue(body, i); // skips the validated entry object
      skipWs(body, i);
      if (i < body.size() && body[i] == ',') {
        ++i;
        continue;
      }
      if (i < body.size() && body[i] == '}') {
        ++i;
        break;
      }
      return fail("bad files separator");
    }
  }
  size_t kv = 0;
  if (!findField(body, 0, "skipped_special", kv)) {
    return fail("missing skipped_special");
  }
  {
    const auto f = readField(body, kv);
    if (!f.found || !f.isNumber || f.num < 0) {
      return fail("bad skipped_special");
    }
    out.skippedSpecial = static_cast<long>(f.num);
  }
  // Strict tail: value, optional ws, '}', optional ws, end (key order is
  // not enforced, but shape and types are, and trailing garbage is refused).
  size_t t = kv;
  skipValue(body, t);
  skipWs(body, t);
  if (t >= body.size() || body[t] != '}') {
    return fail("bad tail");
  }
  ++t;
  skipWs(body, t);
  if (t != body.size()) {
    return fail("trailing garbage");
  }
  return true;
}

} // namespace snowglobe::diff

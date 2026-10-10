// Recorded-run loader + replay matcher (ADR-0010).
#include "replay_store.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../proxy/base64.hpp"
#include "../util/sha256.hpp"
#include "../util/string_util.hpp"
#include "replay_match.hpp"

namespace snowglobe::replay {
namespace {

uint64_t nowUs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

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

std::string jsonStr(const JsonValue& v, const std::string& key, const std::string& dflt) {
  const auto it = v.fields.find(key);
  if (it == v.fields.end() || it->second.type != JsonValue::Type::String) {
    return dflt;
  }
  return it->second.str;
}

long jsonLong(const JsonValue& v, const std::string& key, long dflt) {
  const auto it = v.fields.find(key);
  if (it == v.fields.end() || it->second.type != JsonValue::Type::Number) {
    return dflt;
  }
  try {
    return std::stol(it->second.number);
  } catch (...) {
    return dflt;
  }
}

// Confine a blob rel path to runDir (same rule as link/view: no leading
// slash, no ".." segment, must resolve INSIDE runDir). The canonical
// check matters: a symlink inside llm/ pointing at /etc/shadow must not
// become a served "model response" — replaying an untrusted run would
// otherwise exfiltrate host files to the traced agent. Rejects loudly.
bool confined(const std::string& runDir, const std::string& rel, std::string& absOut) {
  namespace fs = std::filesystem;
  if (runDir.empty() || rel.empty() || rel[0] == '/') {
    return false;
  }
  std::istringstream ss(rel);
  std::string seg;
  while (std::getline(ss, seg, '/')) {
    if (seg == "..") {
      return false;
    }
  }
  std::error_code ec;
  const fs::path root = fs::canonical(fs::path(runDir), ec);
  if (ec) {
    return false;
  }
  const fs::path cand = fs::canonical(fs::path(runDir) / rel, ec);
  if (ec || !fs::is_regular_file(cand, ec) || ec) {
    return false;
  }
  // Containment: the canonical blob path must stay under the canonical
  // run root (prefix match on path elements, not a string prefix).
  auto rIt = root.begin();
  auto cIt = cand.begin();
  for (; rIt != root.end() && cIt != cand.end(); ++rIt, ++cIt) {
    if (*rIt != *cIt) {
      return false;
    }
  }
  if (rIt != root.end()) {
    return false;
  }
  absOut = cand.string();
  return true;
}

} // namespace

bool ReplayStore::load(const std::string& runDir, std::string& err) {
  runDir_ = runDir;
  unrecordedPath_ = runDir + "/replay.unrecorded.jsonl";
  turns_.clear();

  std::string eventsText;
  if (!readFile(runDir + "/events.jsonl", eventsText)) {
    err = "cannot read " + runDir + "/events.jsonl";
    return false;
  }
  // One llm.response event per recorded turn (id/status/req/res/idx).
  // Lines that are not llm.response are skipped (never parsed); corrupt
  // llm.response lines fail loud (a torn trace must not replay silently).
  std::istringstream lines(eventsText);
  std::string line;
  struct ResRef {
    long id = -1;
    int status = 0;
    std::string req, res, idx;
  };
  std::vector<ResRef> refs;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.find("\"llm.response\"") == std::string::npos) {
      continue;
    }
    JsonValue v;
    std::string perr;
    if (!parseJson(line, v, perr)) {
      err = "corrupt llm.response line (" + perr + ")";
      return false;
    }
    const auto ev = v.fields.find("ev");
    if (ev == v.fields.end() || ev->second.type != JsonValue::Type::String ||
        ev->second.str != "llm.response") {
      continue;
    }
    ResRef r;
    r.id = jsonLong(v, "id", -1);
    r.status = static_cast<int>(jsonLong(v, "status", 0));
    r.req = jsonStr(v, "req", "");
    r.res = jsonStr(v, "res", "");
    r.idx = jsonStr(v, "idx", "");
    if (r.id < 0 || r.req.empty() || r.res.empty() || r.idx.empty()) {
      err = "llm.response event missing id/req/res/idx";
      return false;
    }
    refs.push_back(r);
  }

  for (const ResRef& r : refs) {
    RecordedTurn t;
    t.id = r.id;
    t.status = r.status;
    std::string reqAbs, resAbs, idxAbs;
    if (!confined(runDir, r.req, reqAbs)) {
      err = "request blob escapes run or is missing: " + r.req;
      return false;
    }
    if (!confined(runDir, r.res, resAbs)) {
      err = "response blob escapes run or is missing: " + r.res;
      return false;
    }
    if (!confined(runDir, r.idx, idxAbs)) {
      err = "idx blob escapes run or is missing: " + r.idx;
      return false;
    }
    std::string envText;
    if (!readFile(reqAbs, envText)) {
      err = "cannot read " + reqAbs;
      return false;
    }
    JsonValue env;
    std::string eerr;
    if (!parseJson(envText, env, eerr) || env.type != JsonValue::Type::Object) {
      err = "corrupt request envelope " + r.req + " (" + eerr + ")";
      return false;
    }
    t.method = jsonStr(env, "method", "");
    t.path = jsonStr(env, "path", "");
    t.provider = jsonStr(env, "provider", "unknown");
    std::string bodyEnc = jsonStr(env, "body_encoding", "utf8");
    std::string bodyRaw = jsonStr(env, "body", "");
    if (t.method.empty() || t.path.empty()) {
      err = "request envelope missing method/path: " + r.req;
      return false;
    }
    if (bodyEnc == "base64") {
      if (!proxy::base64Decode(bodyRaw, t.reqBody, false)) {
        err = "request envelope has bad base64 body: " + r.req;
        return false;
      }
    } else {
      t.reqBody = bodyRaw;
    }
    if (!readFile(resAbs, t.resBody)) {
      err = "cannot read " + resAbs;
      return false;
    }
    const size_t dot = resAbs.rfind('.');
    t.resExt = (dot == std::string::npos) ? ".bin" : resAbs.substr(dot);
    t.resSha256 = util::sha256Hex(t.resBody);
    // Chunk bounds from the idx: off = bytes received BEFORE that chunk
    // (proxy.cpp writes off, then appends). Empty idx = one whole chunk.
    std::string idxText;
    if (!readFile(idxAbs, idxText)) {
      err = "cannot read " + idxAbs;
      return false;
    }
    std::vector<uint64_t> offs;
    std::vector<uint64_t> stamps;
    std::istringstream idxLines(idxText);
    std::string iline;
    while (std::getline(idxLines, iline)) {
      if (!iline.empty() && iline.back() == '\r') {
        iline.pop_back();
      }
      if (iline.empty()) {
        continue;
      }
      JsonValue iv;
      std::string ierr;
      if (!parseJson(iline, iv, ierr) || iv.type != JsonValue::Type::Object) {
        err = "corrupt idx line in " + r.idx + " (" + ierr + ")";
        return false;
      }
      offs.push_back(static_cast<uint64_t>(jsonLong(iv, "off", -1)));
      stamps.push_back(static_cast<uint64_t>(jsonLong(iv, "ts_us", 0)));
    }
    for (size_t k = 0; k < offs.size(); ++k) {
      if (offs[k] > t.resBody.size() || (k > 0 && offs[k] < offs[k - 1])) {
        err = "idx bounds out of range in " + r.idx;
        return false;
      }
    }
    if (!offs.empty() && offs[0] != 0) {
      err = "idx does not start at 0 in " + r.idx;
      return false;
    }
    if (offs.empty()) {
      t.chunks.push_back(t.resBody);
    } else {
      for (size_t k = 0; k < offs.size(); ++k) {
        const size_t lo = static_cast<size_t>(offs[k]);
        const size_t hi =
            (k + 1 < offs.size()) ? static_cast<size_t>(offs[k + 1]) : t.resBody.size();
        t.chunks.emplace_back(t.resBody.data() + lo, hi - lo);
      }
      for (size_t k = 1; k < stamps.size(); ++k) {
        t.gapsUs.push_back(stamps[k] > stamps[k - 1] ? stamps[k] - stamps[k - 1] : 0);
      }
    }
    t.hash = requestHash(t.provider, t.method, t.path, t.reqBody);
    turns_.push_back(std::move(t));
  }
  return true;
}

ReplayMatch ReplayStore::match(const std::string& provider, const std::string& method,
                               const std::string& path, const std::string& body) {
  const std::string h = requestHash(provider, method, path, body);
  std::lock_guard<std::mutex> lk(mu_);
  // (a) primary: first unused recorded request with the same hash.
  for (size_t k = 0; k < turns_.size(); ++k) {
    if (!turns_[k].used && turns_[k].hash == h) {
      turns_[k].used = true;
      served_.fetch_add(1);
      return ReplayMatch{true, true, turns_[k].id, k};
    }
  }
  // (b) fallback: next unused recorded request for the same endpoint
  // (provider + method + path), in record order. Order-preserving by
  // construction; the body mismatch is surfaced by the replay compare
  // (never silent). Method is load-bearing here too: a GET probe must
  // not fall back onto a POST's response on the same route (P1).
  for (size_t k = 0; k < turns_.size(); ++k) {
    if (!turns_[k].used && turns_[k].provider == provider && turns_[k].method == method &&
        turns_[k].path == path) {
      turns_[k].used = true;
      served_.fetch_add(1);
      return ReplayMatch{true, false, turns_[k].id, k};
    }
  }
  // MISS: loud failure. Log first (best-effort; a log failure must not
  // mask the 502 the client is about to get), then count.
  {
    std::ofstream log(unrecordedPath_, std::ios::app | std::ios::binary);
    if (log) {
      log << "{\"ts_us\":" << nowUs() << ",\"method\":" << util::jsonEscape(method)
          << ",\"path\":" << util::jsonEscape(path)
          << ",\"provider\":" << util::jsonEscape(provider) << ",\"body_sha256\":\""
          << util::sha256Hex(body) << "\"" << ",\"body_bytes\":" << body.size()
          << ",\"reason\":\"no-recorded-match\"}\n";
    }
  }
  unrecorded_.fetch_add(1);
  return ReplayMatch{};
}

} // namespace snowglobe::replay

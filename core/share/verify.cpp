// Trace integrity verifier (ADR-0011).
#include "verify.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../replay/replay_match.hpp"
#include "../util/sha256.hpp"

namespace snowglobe::share {
namespace {

namespace fs = std::filesystem;

constexpr size_t kErrCap = 10;

void pushErr(VerifyResult& out, const std::string& e) {
  if (out.errors.size() < kErrCap) {
    out.errors.push_back(e);
  }
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

bool isHex64(const std::string& s) {
  if (s.size() != 64) {
    return false;
  }
  for (char c : s) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) {
      return false;
    }
  }
  return true;
}

// Tail-anchored chain-field parse (no JSON lib needed — the writer
// appends chain fields in a fixed suffix shape):
//   <payload...>,\"seq\":N,\"prev_hash\":\"P\",\"hash\":\"H\"}
// Returns false on any shape deviation (torn/forged lines fail loud).
bool parseChainTail(const std::string& line, long& seq, std::string& prev, std::string& hash,
                    std::string& withSeq) {
  if (line.empty() || line.back() != '}') {
    return false;
  }
  // Suffix: ,"hash":"<64hex>"} — 1+9+64+1+1 = 75 chars, tail-anchored.
  // (The writer appends chain fields after all payload content, so the
  // LAST occurrence is unambiguously the chain field.)
  const size_t hClose = line.size() - 1; // final '}'
  if (hClose < 74 || line.compare(hClose - 74, 9, ",\"hash\":\"", 9) != 0 ||
      line[hClose - 1] != '"') {
    return false;
  }
  hash = line.substr(hClose - 74 + 9, 64);
  if (!isHex64(hash)) {
    return false;
  }
  withSeq = line.substr(0, hClose - 74) + "}";
  // withSeq suffix: ,"prev_hash":"P"} (14-char key; P is "0" at
  // genesis, else 64 hex — either way non-empty).
  if (withSeq.size() < 17 || withSeq.back() != '}' ||
      withSeq.compare(withSeq.size() - 2, 2, "\"}", 2) != 0) {
    return false;
  }
  const size_t pEnd = withSeq.size() - 2; // closing quote of P
  const size_t pKey = withSeq.rfind(",\"prev_hash\":\"", pEnd);
  if (pKey == std::string::npos || pKey + 14 >= pEnd) {
    return false;
  }
  prev = withSeq.substr(pKey + 14, pEnd - (pKey + 14));
  if (prev.empty()) {
    return false;
  }
  const std::string head = withSeq.substr(0, pKey) + "}";
  // head suffix: ,"seq":N}
  if (head.size() < 9 || head.back() != '}') {
    return false;
  }
  const size_t sKey = head.rfind(",\"seq\":", head.size() - 2);
  if (sKey == std::string::npos) {
    return false;
  }
  const std::string num = head.substr(sKey + 7, head.size() - 1 - (sKey + 7));
  if (num.empty()) {
    return false;
  }
  for (char c : num) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  try {
    seq = std::stol(num);
  } catch (...) {
    return false;
  }
  return true;
}

// Blob rel path confinement (same rule as link/view/replay store:
// lexical, no leading slash, no "..", canonical containment, regular
// file). Untrusted runs and bundles fail loud here, never escape.
bool confined(const std::string& runDir, const std::string& rel, std::string& absOut) {
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

bool verifyRun(const std::string& runDir, VerifyResult& out, std::string& err) {
  out = VerifyResult();
  std::string manifestText;
  if (!readFile(runDir + "/manifest.json", manifestText)) {
    err = "cannot read " + runDir + "/manifest.json";
    return false;
  }
  replay::JsonValue manifest;
  std::string merr;
  if (!replay::parseJson(manifestText, manifest, merr) ||
      manifest.type != replay::JsonValue::Type::Object) {
    err = "corrupt manifest.json (" + merr + ")";
    return false;
  }
  auto numField = [&](const char* key, long& v) {
    const auto it = manifest.fields.find(key);
    if (it == manifest.fields.end() || it->second.type != replay::JsonValue::Type::Number) {
      return false;
    }
    try {
      v = std::stol(it->second.number);
    } catch (...) {
      return false;
    }
    return true;
  };
  auto strField = [&](const char* key, std::string& v) {
    const auto it = manifest.fields.find(key);
    if (it == manifest.fields.end() || it->second.type != replay::JsonValue::Type::String) {
      return false;
    }
    v = it->second.str;
    return true;
  };
  long wantEvents = -1;
  std::string wantHash;
  if (manifest.fields.count("schema") == 0u) {
    err = "manifest.json has no schema";
    return false;
  }
  if (!numField("event_count", wantEvents) || !strField("last_hash", wantHash)) {
    err = "manifest.json lacks event_count/last_hash";
    return false;
  }

  std::string eventsText;
  if (!readFile(runDir + "/events.jsonl", eventsText)) {
    err = "cannot read " + runDir + "/events.jsonl";
    return false;
  }
  std::string running = "0"; // genesis prev_hash
  long seq = 0;
  long lines = 0;
  std::istringstream in(eventsText);
  std::string line;
  // Response/request blob refs collected for the presence pass below.
  std::vector<std::string> blobRefs;
  std::vector<long> reqIds;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    ++lines;
    long gotSeq = -1;
    std::string prev, hash, withSeq;
    if (!parseChainTail(line, gotSeq, prev, hash, withSeq)) {
      pushErr(out, "events.jsonl:" + std::to_string(lines) + ": malformed chain tail");
      break;
    }
    if (gotSeq != seq) {
      pushErr(out, "events.jsonl:" + std::to_string(lines) + ": seq " + std::to_string(gotSeq) +
                       ", want " + std::to_string(seq));
      break;
    }
    if (prev != running) {
      pushErr(out, "events.jsonl:" + std::to_string(lines) + ": prev_hash mismatch (chain break)");
      break;
    }
    if (util::sha256Hex(running + withSeq) != hash) {
      pushErr(out, "events.jsonl:" + std::to_string(lines) + ": hash mismatch (tampered line)");
      break;
    }
    running = hash;
    ++seq;
    // Cheap kind/id routing for the presence pass (full parse only for
    // llm lines, which are few relative to fs/proc noise).
    if (line.find("\"llm.response\"") != std::string::npos ||
        line.find("\"llm.request\"") != std::string::npos) {
      replay::JsonValue v;
      std::string perr;
      if (!replay::parseJson(line, v, perr) || v.type != replay::JsonValue::Type::Object) {
        pushErr(out, "events.jsonl:" + std::to_string(lines) + ": corrupt llm event");
        break;
      }
      const auto ev = v.fields.find("ev");
      const std::string kind =
          (ev != v.fields.end() && ev->second.type == replay::JsonValue::Type::String)
              ? ev->second.str
              : "";
      auto getStr = [&](const char* key, std::string& sv) {
        const auto it = v.fields.find(key);
        if (it != v.fields.end() && it->second.type == replay::JsonValue::Type::String) {
          sv = it->second.str;
          return true;
        }
        return false;
      };
      long id = -1;
      const auto idIt = v.fields.find("id");
      if (idIt != v.fields.end() && idIt->second.type == replay::JsonValue::Type::Number) {
        try {
          id = std::stol(idIt->second.number);
        } catch (...) {
          id = -1;
        }
      }
      if (kind == "llm.request") {
        if (id >= 0) {
          reqIds.push_back(id);
        }
      } else if (kind == "llm.response") {
        std::string req, res, idx;
        if (!getStr("req", req) || !getStr("res", res) || !getStr("idx", idx)) {
          pushErr(out, "events.jsonl:" + std::to_string(lines) + ": response lacks blob refs");
          break;
        }
        blobRefs.push_back(req);
        blobRefs.push_back(res);
        blobRefs.push_back(idx);
      }
    }
    if (out.errors.size() >= kErrCap) {
      break;
    }
  }
  out.events = lines;
  out.lastHash = running;
  if (out.errors.empty()) {
    if (lines != wantEvents) {
      pushErr(out, "manifest event_count " + std::to_string(wantEvents) +
                       " != " + std::to_string(lines) + " lines");
    }
    if (running != wantHash) {
      pushErr(out, "manifest last_hash mismatch");
    }
  }
  // Blob presence (only when the chain itself is intact — a broken chain
  // already fails; piling blob errors on top adds noise, not signal).
  if (out.errors.empty()) {
    for (const std::string& rel : blobRefs) {
      std::string abs;
      if (!confined(runDir, rel, abs)) {
        pushErr(out, "blob missing or escaping run: " + rel);
        if (out.errors.size() >= kErrCap) {
          break;
        }
      }
    }
    for (long id : reqIds) {
      char buf[16] = {};
      std::snprintf(buf, sizeof(buf), "%04ld", id);
      std::string abs;
      if (!confined(runDir, std::string("llm/") + buf + ".req.json", abs)) {
        pushErr(out, std::string("request envelope missing: llm/") + buf + ".req.json");
        if (out.errors.size() >= kErrCap) {
          break;
        }
      }
    }
  }
  out.ok = out.errors.empty();
  return true;
}

} // namespace snowglobe::share

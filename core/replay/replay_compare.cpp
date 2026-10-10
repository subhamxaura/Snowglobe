// Replay compare (ADR-0010).
#include "replay_compare.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "../util/string_util.hpp"
#include "replay_match.hpp"

namespace snowglobe::replay {
namespace {

constexpr size_t kEvidenceCap = 20;

// normalize.py STRIP plus replay-volatiles: replay_of is the join key
// (like prev_hash), ttfb_ms/total_ms are timing (differ on every faithful
// re-execution — volatile-by-construction, see ADR-0010).
const std::set<std::string>& baseStrip() {
  static const std::set<std::string> s = {"seq",       "ts_us",   "t_ms",    "t_us",
                                          "prev_hash", "hash",    "fd",      "backend",
                                          "replay_of", "ttfb_ms", "total_ms"};
  return s;
}

struct Normalizer {
  std::set<std::string> strip = baseStrip();
  std::map<long long, std::string> pidMap;
  std::map<long long, std::string> tidMap;

  std::string pidOf(long long v) {
    auto it = pidMap.find(v);
    if (it == pidMap.end()) {
      const std::string name = "P" + std::to_string(pidMap.size() + 1);
      pidMap[v] = name;
      return name;
    }
    return it->second;
  }
  std::string tidOf(long long v) {
    auto it = tidMap.find(v);
    if (it == tidMap.end()) {
      const std::string name = "T" + std::to_string(tidMap.size() + 1);
      tidMap[v] = name;
      return name;
    }
    return it->second;
  }

  static bool asLong(const JsonValue& v, long long& out) {
    if (v.type != JsonValue::Type::Number) {
      return false;
    }
    try {
      size_t pos = 0;
      out = std::stoll(v.number, &pos);
      return pos == v.number.size();
    } catch (...) {
      return false;
    }
  }

  JsonValue fix(const JsonValue& v, const std::string& key) {
    if (v.type == JsonValue::Type::Object) {
      JsonValue r;
      r.type = JsonValue::Type::Object;
      for (const auto& kv : v.fields) {
        if (strip.count(kv.first) != 0u) {
          continue;
        }
        r.fields[kv.first] = fix(kv.second, kv.first);
      }
      return r;
    }
    if (v.type == JsonValue::Type::Array) {
      JsonValue r;
      r.type = JsonValue::Type::Array;
      for (const auto& it : v.items) {
        r.items.push_back(fix(it, ""));
      }
      return r;
    }
    if ((key == "pid" || key == "ppid") && v.type == JsonValue::Type::Number) {
      // ppid maps through the same namespace as pid (normalize.py rule).
      long long n = 0;
      if (asLong(v, n)) {
        JsonValue r;
        r.type = JsonValue::Type::String;
        r.str = pidOf(n);
        return r;
      }
    }
    if (key == "tid" && v.type == JsonValue::Type::Number) {
      long long n = 0;
      if (asLong(v, n)) {
        JsonValue r;
        r.type = JsonValue::Type::String;
        r.str = tidOf(n);
        return r;
      }
    }
    if (key == "port" && v.type == JsonValue::Type::Number) {
      JsonValue r;
      r.type = JsonValue::Type::String;
      r.str = "PORT";
      return r;
    }
    if (v.type == JsonValue::Type::String) {
      JsonValue r = v;
      r.str = scrubString(v.str);
      return r;
    }
    return v;
  }

  std::string line(const std::string& raw, bool& ok) {
    JsonValue v;
    std::string err;
    if (!parseJson(raw, v, err) || v.type != JsonValue::Type::Object) {
      ok = false;
      return "";
    }
    ok = true;
    return canonicalJson(fix(v, ""));
  }
};

bool readLines(const std::string& path, std::vector<std::string>& out, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot read " + path;
    return false;
  }
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      out.push_back(line);
    }
  }
  return true;
}

std::string evOf(const JsonValue& v) {
  const auto it = v.fields.find("ev");
  if (it != v.fields.end() && it->second.type == JsonValue::Type::String) {
    return it->second.str;
  }
  return "";
}

bool isFs(const std::string& ev) {
  return ev.compare(0, 3, "fs.") == 0;
}
bool isProc(const std::string& ev) {
  // proc.exec argv sets (+exec_failed); start/exit carry no argv and are
  // scheduling-shaped — the exit CODE is compared separately.
  return ev == "proc.exec" || ev == "proc.exec_failed";
}
bool isNet(const std::string& ev) {
  return ev == "net.connect" || ev == "net.sendto" || ev == "net.bind" || ev == "net.disconnect" ||
         ev == "net.dns";
}

// Order-insensitive multiset diff. Equal -> identical with a count detail;
// else diverged with capped +/- evidence (never silent, never unbounded).
void diffSets(const std::vector<std::string>& a, const std::vector<std::string>& b,
              const std::string& what, CategoryResult& out) {
  std::map<std::string, long> ca, cb;
  for (const auto& s : a) {
    ca[s]++;
  }
  for (const auto& s : b) {
    cb[s]++;
  }
  if (ca == cb) {
    out.status = "identical";
    out.detail = what + ": " + std::to_string(a.size()) + " events match";
    return;
  }
  out.status = "diverged";
  size_t onlyB = 0, onlyA = 0;
  for (const auto& kv : cb) {
    const long have = ca.count(kv.first) != 0u ? ca[kv.first] : 0;
    for (long k = have; k < kv.second && out.replayOnly.size() < kEvidenceCap; ++k) {
      out.replayOnly.push_back("+ " + kv.first);
    }
    onlyB += static_cast<size_t>(kv.second - have > 0 ? kv.second - have : 0);
  }
  for (const auto& kv : ca) {
    const long have = cb.count(kv.first) != 0u ? cb[kv.first] : 0;
    for (long k = have; k < kv.second && out.originalOnly.size() < kEvidenceCap; ++k) {
      out.originalOnly.push_back("- " + kv.first);
    }
    onlyA += static_cast<size_t>(kv.second - have > 0 ? kv.second - have : 0);
  }
  out.detail = what + ": " + std::to_string(onlyB) + " replay-only, " + std::to_string(onlyA) +
               " original-only (capped evidence)";
}

long eventId(const JsonValue& v) {
  const auto it = v.fields.find("id");
  if (it != v.fields.end() && it->second.type == JsonValue::Type::Number) {
    try {
      return std::stol(it->second.number);
    } catch (...) {
    }
  }
  return -1;
}

} // namespace

std::string ReplayReport::toJson() const {
  using util::jsonEscape;
  auto cat = [&](const CategoryResult& c, const std::string& extra) {
    std::string s = "{\"status\":" + jsonEscape(c.status) + ",\"detail\":" + jsonEscape(c.detail) +
                    extra + ",\"replay_only\":[";
    for (size_t k = 0; k < c.replayOnly.size(); ++k) {
      s += (k > 0 ? "," : "") + jsonEscape(c.replayOnly[k]);
    }
    s += "],\"original_only\":[";
    for (size_t k = 0; k < c.originalOnly.size(); ++k) {
      s += (k > 0 ? "," : "") + jsonEscape(c.originalOnly[k]);
    }
    return s + "]}";
  };
  // P2: the llm section carries the serve-path split (exact = primary
  // hash hit; fallback = order-preserving endpoint serve).
  const std::string llmExtra = ",\"served_exact\":" + std::to_string(servedExact) +
                               ",\"served_fallback\":" + std::to_string(servedFallback);
  std::string s = "{\"version\":1,\"original\":" + jsonEscape(original) +
                  ",\"replay\":" + jsonEscape(replay) +
                  ",\"original_exit\":" + std::to_string(originalExit) +
                  ",\"replay_exit\":" + std::to_string(replayExit) +
                  ",\"turns\":{\"original\":" + std::to_string(origTurns) +
                  ",\"replay\":" + std::to_string(newTurns) +
                  ",\"match\":" + (origTurns == newTurns ? "true" : "false") +
                  "},\"order_matches\":" + (orderMatches ? "true" : "false") +
                  ",\"unrecorded\":" + std::to_string(unrecorded) + ",\"ignores\":[";
  for (size_t k = 0; k < ignores.size(); ++k) {
    s += (k > 0 ? "," : "") + jsonEscape(ignores[k]);
  }
  s += "],\"categories\":{\"llm\":" + cat(llm, llmExtra) + ",\"fs\":" + cat(fs, "") +
       ",\"proc\":" + cat(proc, "") + ",\"net\":" + cat(net, "") + ",\"exit\":" + cat(exitCat, "") +
       "}}";
  return s;
}

bool compareReplay(const std::string& origDir, const std::string& newDir,
                   const CompareOptions& opts, ReplayReport& out, std::string& err) {
  out = ReplayReport();
  out.original = origDir;
  out.replay = newDir;
  out.ignores = opts.ignoreFields;

  std::vector<std::string> origLines, newLines;
  if (!readLines(origDir + "/events.jsonl", origLines, err)) {
    return false;
  }
  if (!readLines(newDir + "/events.jsonl", newLines, err)) {
    return false;
  }

  Normalizer no, nn;
  for (const auto& f : opts.ignoreFields) {
    no.strip.insert(f);
    nn.strip.insert(f);
  }
  // Parse + normalize both streams (corrupt lines fail loud — a torn
  // trace must not compare as "identical").
  struct Ev {
    JsonValue raw;
    std::string norm;
  };
  std::vector<Ev> oe, ne;
  for (const auto& ln : origLines) {
    bool ok = false;
    const std::string n = no.line(ln, ok);
    if (!ok) {
      err = "corrupt event line in " + origDir + "/events.jsonl";
      return false;
    }
    JsonValue v;
    std::string perr;
    parseJson(ln, v, perr);
    oe.push_back({std::move(v), n});
  }
  for (const auto& ln : newLines) {
    bool ok = false;
    const std::string n = nn.line(ln, ok);
    if (!ok) {
      err = "corrupt event line in " + newDir + "/events.jsonl";
      return false;
    }
    JsonValue v;
    std::string perr;
    parseJson(ln, v, perr);
    ne.push_back({std::move(v), n});
  }

  // Root exit codes: proc.start root:true -> pid; leader proc.exit
  // (tid==pid), else last proc.exit for the root pid. Unknown on either
  // side is diverged (loud), never assumed equal.
  auto rootExit = [](const std::vector<Ev>& evs) -> long {
    bool haveRoot = false;
    std::string rootPid, rootTid;
    for (const auto& e : evs) {
      if (evOf(e.raw) != "proc.start") {
        continue;
      }
      const auto rt = e.raw.fields.find("root");
      if (rt != e.raw.fields.end() && rt->second.type == JsonValue::Type::Bool &&
          rt->second.boolean) {
        const auto pid = e.raw.fields.find("pid");
        const auto tid = e.raw.fields.find("tid");
        if (pid != e.raw.fields.end() && tid != e.raw.fields.end()) {
          rootPid = canonicalJson(pid->second);
          rootTid = canonicalJson(tid->second);
          haveRoot = true;
          break;
        }
      }
    }
    if (!haveRoot) {
      return -1;
    }
    long fallback = -1;
    bool saw = false;
    for (const auto& e : evs) {
      if (evOf(e.raw) != "proc.exit") {
        continue;
      }
      const auto pid = e.raw.fields.find("pid");
      if (pid == e.raw.fields.end() || canonicalJson(pid->second) != rootPid) {
        continue;
      }
      const auto code = e.raw.fields.find("code");
      if (code == e.raw.fields.end() || code->second.type != JsonValue::Type::Number) {
        continue; // vanished:true exits carry no code
      }
      long c = -1;
      try {
        c = std::stol(code->second.number);
      } catch (...) {
        continue;
      }
      saw = true;
      fallback = c;
      const auto tid = e.raw.fields.find("tid");
      if (tid != e.raw.fields.end() && canonicalJson(tid->second) == rootTid) {
        return c; // leader exit: authoritative
      }
    }
    return saw ? fallback : -1;
  };
  out.originalExit = rootExit(oe);
  out.replayExit = rootExit(ne);
  if (out.originalExit < 0 || out.replayExit < 0) {
    out.exitCat.status = "diverged";
    out.exitCat.detail = "exit unknown on " + std::string(out.originalExit < 0 ? "original" : "") +
                         std::string(out.originalExit < 0 && out.replayExit < 0 ? "+" : "") +
                         std::string(out.replayExit < 0 ? "replay" : "") + " (no root proc.exit)";
  } else if (out.originalExit == out.replayExit) {
    out.exitCat.status = "identical";
    out.exitCat.detail =
        "exit " + std::to_string(out.replayExit) + " == " + std::to_string(out.originalExit);
  } else {
    out.exitCat.status = "diverged";
    out.exitCat.detail = "exit " + std::to_string(out.replayExit) + " != original " +
                         std::to_string(out.originalExit);
  }

  // Category multisets (order-insensitive; pids/ports/tmp normalized).
  std::vector<std::string> oFs, nFs, oProc, nProc, oNet, nNet;
  std::vector<Ev> oResp, nResp;
  for (const auto& e : oe) {
    const std::string ev = evOf(e.raw);
    if (isFs(ev)) {
      oFs.push_back(e.norm);
    } else if (isProc(ev)) {
      oProc.push_back(e.norm);
    } else if (isNet(ev)) {
      oNet.push_back(e.norm);
    } else if (ev == "llm.response") {
      oResp.push_back(e);
    }
  }
  for (const auto& e : ne) {
    const std::string ev = evOf(e.raw);
    if (isFs(ev)) {
      nFs.push_back(e.norm);
    } else if (isProc(ev)) {
      nProc.push_back(e.norm);
    } else if (isNet(ev)) {
      nNet.push_back(e.norm);
    } else if (ev == "llm.response") {
      nResp.push_back(e);
    }
  }
  diffSets(oFs, nFs, "fs", out.fs);
  diffSets(oProc, nProc, "proc.exec", out.proc);
  diffSets(oNet, nNet, "net", out.net);

  // LLM layer: turn counts, serve mapping (replay_of), per-turn request
  // body equality (normalized hashes from both runs' envelopes), serve
  // order monotonicity, response status/bytes parity. Any 502 without a
  // mapping is an unrecorded call -> llm status unrecorded.
  out.origTurns = static_cast<long>(oResp.size());
  out.newTurns = static_cast<long>(nResp.size());
  auto bodyHash = [](const std::string& dir, long id) -> std::string {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%04ld", id);
    std::string envText;
    std::ifstream f(dir + "/llm/" + buf + ".req.json", std::ios::binary);
    if (!f) {
      return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    envText = ss.str();
    JsonValue env;
    std::string eerr;
    if (!parseJson(envText, env, eerr) || env.type != JsonValue::Type::Object) {
      return "";
    }
    std::string body;
    const auto b = env.fields.find("body");
    if (b != env.fields.end() && b->second.type == JsonValue::Type::String) {
      body = b->second.str;
    }
    // NOTE: base64-wrapped bodies hash in wrapped form (both sides wrap
    // identically, so equality is preserved without a decode step).
    std::string provider, method, path;
    const auto p = env.fields.find("provider");
    if (p != env.fields.end() && p->second.type == JsonValue::Type::String) {
      provider = p->second.str;
    }
    const auto m = env.fields.find("method");
    if (m != env.fields.end() && m->second.type == JsonValue::Type::String) {
      method = m->second.str;
    }
    const auto pa = env.fields.find("path");
    if (pa != env.fields.end() && pa->second.type == JsonValue::Type::String) {
      path = pa->second.str;
    }
    return requestHash(provider, method, path, body);
  };
  std::map<long, std::string> oHash, nHash;
  for (const auto& e : oResp) {
    const long id = eventId(e.raw);
    if (id >= 0) {
      oHash[id] = bodyHash(origDir, id);
    }
  }
  for (const auto& e : nResp) {
    const long id = eventId(e.raw);
    if (id >= 0) {
      nHash[id] = bodyHash(newDir, id);
    }
  }
  long unrec = 0;
  long exact = 0, fallback = 0;
  std::vector<long> serveOrder;
  std::vector<std::string> problems;
  // Response parity keyed by serve mapping (status/bytes/truncated vs the
  // recorded turn that served it).
  auto respField = [](const Ev& e, const std::string& k) -> std::string {
    const auto it = e.raw.fields.find(k);
    return it == e.raw.fields.end() ? "" : canonicalJson(it->second);
  };
  std::map<long, const Ev*> oById;
  for (const auto& e : oResp) {
    oById[eventId(e.raw)] = &e;
  }
  for (const auto& e : nResp) {
    const long nid = eventId(e.raw);
    const auto ro = e.raw.fields.find("replay_of");
    long mapped = -1;
    if (ro != e.raw.fields.end() && ro->second.type == JsonValue::Type::Number) {
      try {
        mapped = std::stol(ro->second.number);
      } catch (...) {
        mapped = -1;
      }
    }
    if (mapped < 0) {
      ++unrec;
      problems.push_back("turn new-" + std::to_string(nid) + ": unrecorded (502, no replay_of)");
      continue;
    }
    serveOrder.push_back(mapped);
    const auto oit = oById.find(mapped);
    if (oit == oById.end()) {
      problems.push_back("turn new-" + std::to_string(nid) +
                         ": replay_of=" + std::to_string(mapped) + " has no original");
      ++fallback; // served, but maps nowhere — cannot be exact
      continue;
    }
    // Serve-path split (P2): equal normalized hashes mean the primary
    // hash match served this turn (the matcher tries primary first, so a
    // fallback-found entry always differs); anything else — including an
    // unreadable envelope — counts as fallback.
    if (!nHash[nid].empty() && nHash[nid] == oHash[mapped]) {
      ++exact;
    } else {
      ++fallback;
    }
    if (nHash[nid].empty() || oHash[mapped].empty()) {
      problems.push_back("turn " + std::to_string(mapped) + ": envelope unreadable");
    } else if (nHash[nid] != oHash[mapped]) {
      problems.push_back("turn " + std::to_string(mapped) + ": request body differs " +
                         "(replayed " + nHash[nid].substr(0, 12) + " vs recorded " +
                         oHash[mapped].substr(0, 12) + ")");
    }
    for (const char* k : {"status", "bytes", "truncated"}) {
      if (respField(e, k) != respField(*oit->second, k)) {
        problems.push_back("turn " + std::to_string(mapped) + ": response " + k + " " +
                           respField(*oit->second, k) + " -> " + respField(e, k));
      }
    }
  }
  out.unrecorded = unrec;
  out.servedExact = exact;
  out.servedFallback = fallback;
  // Serve order: every recorded turn served exactly once (consumption
  // guarantees the multiset), plus relative order preserved wherever the
  // recorded requests DIFFER. Identical requests (same canonical hash)
  // are interchangeable by construction — concurrent replays interleave
  // them freely, and flagging that would be noise, not signal.
  out.orderMatches = true;
  {
    std::set<long> servedSet(serveOrder.begin(), serveOrder.end());
    if (serveOrder.size() != oHash.size()) {
      out.orderMatches = false;
      problems.push_back("served " + std::to_string(serveOrder.size()) + " recorded turns, want " +
                         std::to_string(oHash.size()));
    } else {
      for (size_t a = 0; a < serveOrder.size() && out.orderMatches; ++a) {
        for (size_t b = a + 1; b < serveOrder.size(); ++b) {
          const long ia = serveOrder[a], ib = serveOrder[b];
          if (ia == ib) {
            out.orderMatches = false;
            problems.push_back("recorded turn " + std::to_string(ia) + " served twice");
            break;
          }
          if (ia > ib && oHash[ia] != oHash[ib]) {
            out.orderMatches = false;
            problems.push_back("serve order is not record order (new-" + std::to_string(a) +
                               " got recorded-" + std::to_string(ia) + " after recorded-" +
                               std::to_string(ib) + ")");
            break;
          }
        }
      }
    }
  }
  if (out.origTurns != out.newTurns) {
    problems.push_back("turn count " + std::to_string(out.newTurns) + " != original " +
                       std::to_string(out.origTurns));
  }
  if (unrec > 0) {
    out.llm.status = "unrecorded";
    out.llm.detail = std::to_string(unrec) + " unrecorded call(s) (502)";
  } else if (!problems.empty()) {
    out.llm.status = "diverged";
    out.llm.detail = std::to_string(problems.size()) + " difference(s)";
  } else {
    out.llm.status = "identical";
    out.llm.detail =
        std::to_string(out.newTurns) + "/" + std::to_string(out.origTurns) + " turns served";
  }
  for (const auto& p : problems) {
    if (out.llm.replayOnly.size() >= kEvidenceCap) {
      break;
    }
    out.llm.replayOnly.push_back(p);
  }
  return true;
}

} // namespace snowglobe::replay

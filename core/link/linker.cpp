// Causal linker implementation (algorithm in link.hpp; rationale ADR-0006).
// Thread ownership: pure function of its inputs (no shared state).
#include "link.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>

#include "../util/string_util.hpp"
#include "event_scan.hpp"
#include "tools.hpp"

namespace snowglobe::link {
namespace {

constexpr long long kNoSeq = -1;
constexpr long long kNoTurn = -1;

// Parsed event: raw string ids stay verbatim (numbers stringified) so real
// runs (numeric pids) and normalised fixtures ("P1") share one code path.
struct Ev {
  long long line = 0; // file order
  long long seq = kNoSeq;
  std::string kind;
  std::string pid;
  std::string tid;
  bool procStart = false;
  bool threadStart = false;
  bool procExit = false;
  bool vanished = false;
  bool exec = false;
  std::vector<std::string> argv;
  // llm.*
  bool llmReq = false;
  bool llmRes = false;
  long long llmId = kNoSeq;
  std::string provider;
  std::string method;
  bool hasModel = false; // string model present (null/absent → false)
  long long bytes = 0;
  bool stream = false;
  long long status = kNoSeq;
  std::string resRel;
};

std::string idStr(const RawField& f) {
  if (!f.found) {
    return "";
  }
  if (f.isString) {
    return f.str;
  }
  if (f.isNumber) {
    return std::to_string(f.num);
  }
  return "";
}

bool getNum(const std::string& line, const char* key, long long& out) {
  size_t v = 0;
  if (!findField(line, 0, key, v)) {
    return false;
  }
  const RawField f = readField(line, v);
  if (!f.found || !f.isNumber) {
    return false;
  }
  out = f.num;
  return true;
}

bool getStr(const std::string& line, const char* key, std::string& out) {
  size_t v = 0;
  if (!findField(line, 0, key, v)) {
    return false;
  }
  const RawField f = readField(line, v);
  if (!f.found || !f.isString) {
    return false;
  }
  out = f.str;
  return true;
}

bool getBool(const std::string& line, const char* key, bool& out) {
  size_t v = 0;
  if (!findField(line, 0, key, v)) {
    return false;
  }
  const RawField f = readField(line, v);
  if (!f.found || !f.isBool) {
    return false;
  }
  out = f.boolean;
  return true;
}

bool parseEv(const std::string& line, long long num, Ev& e) {
  e.line = num;
  std::string kind;
  if (!getStr(line, "ev", kind)) {
    return false;
  }
  e.kind = kind;
  long long seq = 0;
  if (getNum(line, "seq", seq)) {
    e.seq = seq;
  }
  size_t v = 0;
  if (findField(line, 0, "pid", v)) {
    e.pid = idStr(readField(line, v));
  }
  if (findField(line, 0, "tid", v)) {
    e.tid = idStr(readField(line, v));
  }
  if (e.kind == "proc.start") {
    e.procStart = true;
    bool thread = false;
    if (getBool(line, "thread", thread) && thread) {
      e.threadStart = true;
    }
  } else if (e.kind == "proc.exit") {
    e.procExit = true;
    bool vanished = false;
    if (getBool(line, "vanished", vanished) && vanished) {
      e.vanished = true;
    }
  } else if (e.kind == "proc.exec") {
    e.exec = true;
    if (findField(line, 0, "argv", v)) {
      const RawField f = readField(line, v);
      if (f.found && f.isArray) {
        getStrArray(line, v, e.argv); // non-string element → empty argv
      }
    }
  } else if (e.kind == "llm.request") {
    e.llmReq = true;
    getNum(line, "id", e.llmId);
    getStr(line, "provider", e.provider);
    getStr(line, "method", e.method);
    std::string model;
    e.hasModel = getStr(line, "model", model);
    getNum(line, "bytes", e.bytes);
    getBool(line, "stream", e.stream);
  } else if (e.kind == "llm.response") {
    e.llmRes = true;
    getNum(line, "id", e.llmId);
    getNum(line, "status", e.status);
    getStr(line, "res", e.resRel);
  }
  return true;
}

bool isProbeReq(const Ev& r) {
  if (r.method == "HEAD") {
    return true;
  }
  return !r.hasModel && r.bytes == 0;
}

// Whitespace-normalised comparison: trim + collapse runs (commands are
// case-sensitive; a "FOO" argv never matches a "foo" tool string).
// Boundary predicate for joined-argv substring hits (R2): a hit counts
// only at string start/end, whitespace, or '/'. In particular "rm" never
// matches "perform_clean" (no boundary around the hit), while "/bin/rm"
// still matches "rm" and "git status" still matches "git status --short".
bool isMatchBoundary(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/';
}
std::string normSpace(const std::string& s) {
  std::string out;
  bool pending = false;
  for (char c : s) {
    const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (ws) {
      pending = !out.empty(); // trim leading; collapse the rest
      continue;
    }
    if (pending) {
      out.push_back(' ');
      pending = false;
    }
    out.push_back(c);
  }
  return out;
}

bool argvMatches(const std::vector<std::string>& argv, const std::string& cmd) {
  const std::string want = normSpace(cmd);
  if (want.empty() || argv.empty()) {
    return false;
  }
  std::string joined;
  for (const std::string& a : argv) {
    const std::string na = normSpace(a);
    if (na == want) {
      return true; // exact element (toy: argv[2] == command)
    }
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += na;
  }
  // Substring hits require token boundaries on both sides (R2).
  for (size_t pos = joined.find(want); pos != std::string::npos; pos = joined.find(want, pos + 1)) {
    const bool left = (pos == 0) || isMatchBoundary(joined[pos - 1]);
    const size_t end = pos + want.size();
    const bool right = (end == joined.size()) || isMatchBoundary(joined[end]);
    if (left && right) {
      return true;
    }
  }
  return false;
}

struct Turn {
  long long id = 0;
  std::string provider = "unknown";
  bool stream = false;
  long long status = 0;
  long long reqKey = 0;
  long long resKey = 0;
  std::vector<std::string> toolIds;
  std::vector<std::string> commands;
};

} // namespace

LinksDoc buildLinks(const std::vector<std::string>& lines, BlobReader blobs) {
  LinksDoc doc;
  std::vector<Ev> evs;
  evs.reserve(lines.size());
  long long num = 0;
  for (const std::string& line : lines) {
    if (line.empty()) {
      continue;
    }
    Ev e;
    if (parseEv(line, num, e)) {
      evs.push_back(e);
    }
    // else: unparseable line skipped (decode failures are tracer events,
    // not linker input errors)
    ++num;
  }

  bool allSeq = true;
  for (const Ev& e : evs) {
    if (e.seq == kNoSeq) {
      allSeq = false;
      break;
    }
  }
  auto key = [&](const Ev& e) -> long long { return allSeq ? e.seq : e.line; };

  // Pass 1: requests by id (last wins, mirroring viewer buildTurns), then
  // one turn per response in key order (probes excluded).
  std::map<long long, const Ev*> reqs;
  for (const Ev& e : evs) {
    if (e.llmReq && e.llmId != kNoSeq) {
      reqs[e.llmId] = &e;
    }
  }
  struct Resp {
    long long key = 0;
    const Ev* e = nullptr;
  };
  std::vector<Resp> resps;
  for (const Ev& e : evs) {
    if (e.llmRes && e.status != kNoSeq) {
      resps.push_back({key(e), &e});
    }
  }
  std::stable_sort(resps.begin(), resps.end(),
                   [](const Resp& a, const Resp& b) { return a.key < b.key; });

  std::vector<Turn> turns;
  for (const Resp& r : resps) {
    const Ev* q = nullptr;
    const auto it = reqs.find(r.e->llmId);
    if (it != reqs.end()) {
      q = it->second;
    }
    if (q != nullptr && isProbeReq(*q)) {
      ++doc.probesExcluded;
      continue;
    }
    Turn t;
    t.id = r.e->llmId;
    t.resKey = r.key;
    t.reqKey = (q != nullptr) ? key(*q) : r.key;
    t.status = r.e->status;
    if (q != nullptr) {
      if (!q->provider.empty()) {
        t.provider = q->provider;
      }
      t.stream = q->stream;
    }
    if (q != nullptr && !r.e->resRel.empty() && blobs) {
      std::string body;
      if (blobs(r.e->resRel, body)) {
        for (const ToolCall& tc : extractTools(body, t.provider)) {
          if (!tc.id.empty()) {
            t.toolIds.push_back(tc.id);
          }
          if (!tc.command.empty()) {
            t.commands.push_back(tc.command);
          }
        }
      }
      // else: unreadable blob → no tools (partial traces stay linkable)
    }
    turns.push_back(t);
  }
  std::stable_sort(turns.begin(), turns.end(),
                   [](const Turn& a, const Turn& b) { return a.reqKey < b.reqKey; });
  // Res-sorted span index (out-of-order safe): turns stay in reqKey order
  // for deterministic output, but span boundaries live in res-completion
  // order. resOrder[j] = turns index of the j-th completed response
  // (sorted by resKey, ties by reqKey). spanAt binary-searches resOrder
  // and returns the owning turns index (or -1 pre-first-response), so a
  // concurrent trace completing res2 before res1 attributes correctly
  // instead of tripping the old req==res monotonicity assert (retired).
  std::vector<size_t> resOrder(turns.size());
  for (size_t i = 0; i < turns.size(); ++i) {
    resOrder[i] = i;
  }
  std::stable_sort(resOrder.begin(), resOrder.end(), [&](size_t a, size_t b) {
    if (turns[a].resKey != turns[b].resKey) {
      return turns[a].resKey < turns[b].resKey;
    }
    return turns[a].reqKey < turns[b].reqKey;
  });
  doc.turns = static_cast<long long>(turns.size());

  // Pass 2: attribute every non-llm event in file order.
  // Res-partition (R1): in res-completion order, span j =
  // [resOrder[j].resKey, resOrder[j+1].resKey); tail runs to +inf. The
  // owner is the last *completed* response: an event after request N+1
  // but before response N+1 still belongs to turn N (an in-flight
  // request does not move the boundary).
  std::vector<TurnLinks> out(turns.size());
  for (size_t i = 0; i < turns.size(); ++i) {
    out[i].turn = turns[i].id;
    out[i].req = turns[i].reqKey;
    out[i].res = turns[i].resKey;
    out[i].tools = turns[i].toolIds;
  }
  auto spanAt = [&](long long k) -> long long {
    long long lo = 0;
    long long hi = static_cast<long long>(resOrder.size());
    while (lo < hi) {
      const long long mid = (lo + hi) / 2;
      if (turns[resOrder[static_cast<size_t>(mid)]].resKey <= k) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo == 0) {
      return -1; // before the first completed response
    }
    return static_cast<long long>(resOrder[static_cast<size_t>(lo - 1)]);
  };

  std::map<std::string, long long> birth; // pid → turn index at proc.start (-1 pre-turn)
  std::set<std::string> dead;
  std::set<std::string> threads; // "pid|tid" seen with thread:true
  std::vector<Ev> ordered = evs;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [&](const Ev& a, const Ev& b) { return key(a) < key(b); });

  for (const Ev& e : ordered) {
    const long long k = key(e);
    const bool threadExit = e.procExit && threads.find(e.pid + "|" + e.tid) != threads.end();
    if (e.procStart) {
      if (!e.threadStart) {
        birth[e.pid] = spanAt(k);
        dead.erase(e.pid);
      } else {
        threads.insert(e.pid + "|" + e.tid);
        if (birth.find(e.pid) == birth.end()) {
          birth[e.pid] = spanAt(k); // defensive: unseen thread pid
        }
      }
      // proc.start itself is process-tree evidence: attributed by span below.
    }
    // Process death is bookkept AFTER attributing the exit event itself,
    // so a background child that outlives its turn keeps lineage for the
    // exit. Thread exits and vanished exec remnants never kill a lineage.
    const bool dying = e.procExit && !e.vanished && !threadExit;
    if (e.llmReq || e.llmRes) {
      continue; // turn skeleton: referenced, never attributed
    }
    long long idx = kNoTurn;
    std::string basis;
    const auto it = birth.find(e.pid);
    if (!e.pid.empty() && it != birth.end() && it->second >= 0 && dead.find(e.pid) == dead.end()) {
      idx = it->second;
      basis = "lineage";
    } else {
      idx = spanAt(k);
      if (idx >= 0) {
        basis = "window";
      }
    }
    if (idx < 0) {
      if (!out.empty()) {
        out[0].unattributed.push_back({k, "pre-turn"});
        ++doc.unattributed;
        ++doc.reasonCounts["pre-turn"];
      }
      continue;
    }
    TurnLinks& tl = out[static_cast<size_t>(idx)];
    if (e.exec && !turns[static_cast<size_t>(idx)].commands.empty()) {
      bool hit = false;
      for (const std::string& c : turns[static_cast<size_t>(idx)].commands) {
        if (argvMatches(e.argv, c)) {
          hit = true;
          break;
        }
      }
      if (hit) {
        basis = "argv-match"; // upgrade: bodies confirm the window/lineage
      }
    }
    tl.attributed.push_back({k, basis, "high"});
    ++doc.attributed;
    ++doc.basisCounts[basis];
    if (dying) {
      dead.insert(e.pid); // pid reuse starts a fresh lineage (proc.start)
      birth.erase(e.pid);
    }
  }

  // Canonical render: fixed key order, compact, seq-sorted arrays.
  using snowglobe::util::jsonEscape;
  std::ostringstream ss;
  ss << "{\"version\":1,\"turns\":[";
  for (size_t i = 0; i < out.size(); ++i) {
    const TurnLinks& tl = out[i];
    if (i > 0) {
      ss << ",";
    }
    ss << "{\"turn\":" << tl.turn << ",\"llm\":{\"req\":" << tl.req << ",\"res\":" << tl.res
       << ",\"tools\":[";
    for (size_t j = 0; j < tl.tools.size(); ++j) {
      if (j > 0) {
        ss << ",";
      }
      ss << jsonEscape(tl.tools[j]);
    }
    ss << "]},\"attributed\":[";
    for (size_t j = 0; j < tl.attributed.size(); ++j) {
      if (j > 0) {
        ss << ",";
      }
      ss << "{\"basis\":" << jsonEscape(tl.attributed[j].basis)
         << ",\"confidence\":" << jsonEscape(tl.attributed[j].confidence)
         << ",\"seq\":" << tl.attributed[j].seq << "}";
    }
    ss << "],\"unattributed\":[";
    for (size_t j = 0; j < tl.unattributed.size(); ++j) {
      if (j > 0) {
        ss << ",";
      }
      ss << "{\"reason\":" << jsonEscape(tl.unattributed[j].reason)
         << ",\"seq\":" << tl.unattributed[j].seq << "}";
    }
    ss << "]}";
  }
  ss << "]}";
  doc.json = ss.str();
  return doc;
}

} // namespace snowglobe::link

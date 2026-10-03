// snowglobe view — localhost server for the embedded viewer + trace files.
// Thread ownership: httplib pool threads run the handlers; all shared state
// (embedded table, run source, seq index, cached summary) is read-only after
// start. No trace is written by view (read-only).
#include "view.hpp"

#ifdef SNOWGLOBE_HAS_VIEWER
#include "snowglobe_viewer_data.hpp"
#endif

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace snowglobe::view {
namespace {

constexpr int kExUsage = 64;
constexpr int kExUnavailable = 69;
constexpr int kExSoftware = 70;

// /api/events paging bound (spec: seq-indexed, at most 5000/page).
constexpr long kMaxPage = 5000;
constexpr long kDefaultPage = 1000;

#ifdef SNOWGLOBE_HAS_VIEWER
bool startsWith(const std::string& s, const std::string& pre) {
  return s.compare(0, pre.size(), pre) == 0;
}

std::string mimeFor(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  const std::string ext = dot == std::string::npos ? "" : path.substr(dot);
  if (ext == ".html") {
    return "text/html; charset=utf-8";
  }
  if (ext == ".js" || ext == ".mjs") {
    return "text/javascript; charset=utf-8";
  }
  if (ext == ".css") {
    return "text/css; charset=utf-8";
  }
  if (ext == ".json" || ext == ".map" || ext == ".idx") {
    return "application/json";
  }
  if (ext == ".jsonl") {
    return "application/x-ndjson";
  }
  if (ext == ".svg") {
    return "image/svg+xml";
  }
  if (ext == ".png") {
    return "image/png";
  }
  if (ext == ".ico") {
    return "image/x-icon";
  }
  if (ext == ".txt" || ext == ".log" || ext == ".sse") {
    return "text/plain; charset=utf-8";
  }
  return "application/octet-stream";
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

#ifndef _WIN32
bool haveXdgOpen() {
  if (const char* path = ::getenv("PATH")) {
    std::istringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
      if (dir.empty()) {
        continue;
      }
      if (::access((dir + "/xdg-open").c_str(), X_OK) == 0) {
        return true;
      }
    }
  }
  return false;
}

void launchBrowser(const std::string& url) {
  // Double fork: the intermediate child is reaped immediately, the
  // grandchild execs xdg-open and is adopted by init — no zombies, no
  // blocking the server. Best effort: failures stay silent on purpose
  // (the URL is already printed).
  const pid_t a = ::fork();
  if (a < 0) {
    return;
  }
  if (a == 0) {
    if (::fork() == 0) {
      ::execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
      _exit(0);
    }
    _exit(0);
  }
  int status = 0;
  while (::waitpid(a, &status, 0) < 0 && errno == EINTR) {
  }
}
#endif

// ---- minimal JSON field scanners (no provider parsing in C++; the proxy
// already stores raw blobs and the viewer owns interpretation). Tolerant of
// `"k":v` and `"k" : v` alike: normalised fixtures pretty-print with
// spaces while live traces pack tightly. Escapes inside strings are NOT
// decoded (only `ev`/path-ish fields are read; a `\"` inside would
// truncate — acceptable for summary/host attribution, never for blobs). ----

const std::string* valueAt(const std::string& line, const std::string& key, size_t& pos) {
  const std::string pat = "\"" + key + "\"";
  const size_t at = line.find(pat);
  if (at == std::string::npos) {
    return nullptr;
  }
  size_t i = at + pat.size();
  while (i < line.size() && line[i] == ' ') {
    ++i;
  }
  if (i >= line.size() || line[i] != ':') {
    return nullptr;
  }
  ++i;
  while (i < line.size() && line[i] == ' ') {
    ++i;
  }
  if (i >= line.size()) {
    return nullptr;
  }
  pos = i;
  return &line;
}

std::optional<std::string> strField(const std::string& line, const std::string& key) {
  size_t i = 0;
  if (valueAt(line, key, i) == nullptr || line[i] != '"') {
    return std::nullopt;
  }
  const size_t begin = i + 1;
  const size_t end = line.find('"', begin);
  if (end == std::string::npos) {
    return std::nullopt;
  }
  return line.substr(begin, end - begin);
}

std::optional<long long> intField(const std::string& line, const std::string& key) {
  size_t i = 0;
  if (valueAt(line, key, i) == nullptr) {
    return std::nullopt;
  }
  bool neg = false;
  if (line[i] == '-') {
    neg = true;
    ++i;
  }
  if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i]))) {
    return std::nullopt;
  }
  long long v = 0;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
    v = v * 10 + (line[i] - '0');
    ++i;
  }
  return neg ? -v : v;
}

bool wordField(const std::string& line, const std::string& key, const std::string& word) {
  size_t i = 0;
  if (valueAt(line, key, i) == nullptr) {
    return false;
  }
  return line.compare(i, word.size(), word) == 0;
}

bool trueField(const std::string& line, const std::string& key) {
  return wordField(line, key, "true");
}

// ---- run source: a .sgr directory, or a bare events.jsonl file ----

struct RunSource {
  bool isFile = false; // bare events.jsonl, no run dir (no blobs)
  std::string runDir;  // canonical run dir (dir mode only)
  std::string eventsPath;
  std::string manifestText;                // raw manifest.json, or synthesized for bare files
  std::vector<unsigned long long> offsets; // byte offset of each event line
};

bool buildIndex(RunSource& rs, std::string& err) {
  std::ifstream f(rs.eventsPath, std::ios::binary);
  if (!f) {
    err = "cannot read " + rs.eventsPath;
    return false;
  }
  std::string line;
  unsigned long long off = 0;
  while (std::getline(f, line)) {
    rs.offsets.push_back(off);
    off += static_cast<unsigned long long>(line.size()) + 1;
  }
  if (f.bad()) {
    err = "error reading " + rs.eventsPath;
    return false;
  }
  return true;
}

std::string synthesizeManifest(const std::string& eventsPath, size_t count) {
  std::ostringstream ss;
  ss << "{\"schema\":0,\"snowglobe_version\":\"unknown\",\"started\":null,\"finished\":null,"
     << "\"cmd\":[\"";
  for (char c : eventsPath) {
    if (c == '"' || c == '\\') {
      ss << '\\';
    }
    ss << c;
  }
  ss << "\"],\"cwd\":\"\",\"project\":\"\",\"kernel\":\"\",\"tracer\":\"\",\"isolate\":{},"
     << "\"env_fingerprint\":\"\",\"event_count\":" << count << ",\"last_hash\":\"\","
     << "\"file_hashes\":{},\"synthesized\":true}";
  return ss.str();
}

// Resolve a blob path strictly inside the run dir. Rejects lexical escapes
// ("..", leading "/") AND symlink escapes (canonical path must stay under
// the canonical run dir). Missing files and directories are 404, same as
// escapes (no existence oracle).
bool resolveBlob(const RunSource& rs, const std::string& rel, std::string& absOut) {
  namespace fs = std::filesystem;
  if (rs.isFile || rel.empty() || rel[0] == '/') {
    return false;
  }
  {
    std::istringstream ss(rel);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
      if (seg == "..") {
        return false;
      }
    }
  }
  std::error_code ec;
  const fs::path cand = fs::path(rs.runDir) / rel;
  if (!fs::is_regular_file(cand, ec) || ec) {
    return false;
  }
  fs::path canon = fs::canonical(cand, ec);
  if (ec) {
    return false;
  }
  const std::string base = rs.runDir;
  const std::string got = canon.string();
  if (got.size() <= base.size() || got.compare(0, base.size(), base) != 0 ||
      got[base.size()] != '/') {
    return false;
  }
  absOut = got;
  return true;
}

bool parseBound(const std::string& s, long def, long& out) {
  if (s.empty()) {
    out = def;
    return true;
  }
  if (s.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  try {
    out = std::stol(s);
  } catch (...) {
    return false;
  }
  return out >= 0;
}

// Slice [from, to) of the indexed events as a JSON array body (lines are
// already JSON; embedded verbatim, no re-parse).
std::string eventsSlice(const RunSource& rs, long from, long to) {
  std::ifstream f(rs.eventsPath, std::ios::binary);
  std::string body;
  body += "{\"from\":";
  body += std::to_string(from);
  body += ",\"to\":";
  body += std::to_string(to);
  body += ",\"total\":";
  body += std::to_string(rs.offsets.size());
  body += ",\"events\":[";
  std::string line;
  for (long i = from; i < to; ++i) {
    f.seekg(static_cast<std::streamoff>(rs.offsets[static_cast<size_t>(i)]));
    std::getline(f, line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (i > from) {
      body += ',';
    }
    body += line;
  }
  body += "]}";
  return body;
}

std::string hostKeyFor(const std::string& line, std::string& bucket) {
  if (auto p = strField(line, "path")) {
    if (strField(line, "family") == "unix") {
      bucket = "unix";
      return *p;
    }
  }
  if (auto ip = strField(line, "ip")) {
    bucket = "tcp";
    std::string key = *ip;
    if (auto port = intField(line, "port")) {
      key += ":" + std::to_string(*port);
    }
    return key;
  }
  // Legacy formatted addr fallback (pre-structured goldens use PORT).
  if (auto addr = strField(line, "addr")) {
    bucket = (addr->rfind("unix:", 0) == 0) ? "unix" : "tcp";
    return *addr;
  }
  bucket = "other";
  return "unknown";
}

// One pass over events.jsonl: counts by kind, turns (probe requests with no
// model or HEAD method excluded; every other response — incl. non-2xx — is a
// turn), error turns, processes, hosts (tcp host:port, unix separate,
// disconnects distinct), files written, t_ms duration.
std::string buildSummary(const RunSource& rs) {
  std::map<std::string, long long> kinds;
  std::map<long long, std::string> reqMethod;
  std::map<long long, bool> reqHasModel;
  std::map<long long, long long> reqBytes;
  std::set<std::string> pids;
  std::map<std::string, long long> tcpHosts;
  std::map<std::string, long long> unixSockets;
  long long disconnects = 0;
  std::set<std::string> written;
  long long deleted = 0;
  long long renamed = 0;
  long long responses = 0;
  long long errorTurns = 0;
  long long probes = 0;
  long long tMin = -1;
  long long tMax = 0;

  std::ifstream f(rs.eventsPath, std::ios::binary);
  std::string line;
  // First pass: request methods/models (probe classification needs the
  // request paired to each response id).
  std::vector<std::string> lines;
  lines.reserve(rs.offsets.size());
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  for (const auto& ln : lines) {
    auto ev = strField(ln, "ev");
    if (!ev) {
      continue;
    }
    kinds[*ev]++;
    if (auto t = intField(ln, "t_ms")) {
      if (tMin < 0 || *t < tMin) {
        tMin = *t;
      }
      if (*t > tMax) {
        tMax = *t;
      }
    }
    if (*ev == "llm.request") {
      if (auto id = intField(ln, "id")) {
        reqMethod[*id] = strField(ln, "method").value_or("");
        // No model == no model: strField returns nullopt both for
        // "model":null and for an absent key — matching model.ts
        // (str(e["model"]) ?? null). An absent key used to read as
        // "has model", splitting probes from the TS turn layer.
        reqHasModel[*id] = strField(ln, "model").has_value();
        reqBytes[*id] = intField(ln, "bytes").value_or(0);
      }
    } else if (*ev == "proc.start" || *ev == "proc.exec" || *ev == "proc.exit") {
      if (auto pid = strField(ln, "pid")) {
        pids.insert(*pid);
      } else if (auto pid = intField(ln, "pid")) {
        pids.insert(std::to_string(*pid));
      }
    } else if (*ev == "net.disconnect") {
      disconnects++;
    } else if (*ev == "net.connect" || *ev == "net.sendto" || *ev == "net.bind") {
      std::string bucket;
      const std::string key = hostKeyFor(ln, bucket);
      if (bucket == "unix") {
        unixSockets[key]++;
      } else if (bucket == "tcp") {
        tcpHosts[key]++;
      }
    } else if (*ev == "fs.open") {
      if (trueField(ln, "write")) {
        if (auto p = strField(ln, "path")) {
          written.insert(*p);
        }
      }
    } else if (*ev == "fs.unlink" || *ev == "fs.rmdir") {
      deleted++;
    } else if (*ev == "fs.rename") {
      renamed++;
    }
  }
  for (const auto& ln : lines) {
    if (strField(ln, "ev") != "llm.response") {
      continue;
    }
    auto id = intField(ln, "id");
    auto status = intField(ln, "status");
    if (!id || !status) {
      continue;
    }
    // Probe = HEAD pre-flight, or a model-less request with no body
    // (e.g. SDK health checks). A model-less POST *with* a body (Gemini
    // generateContent carries the model in the path, not the JSON) is a
    // real turn. Responses without any recorded request count as turns.
    bool probe = false;
    if (const auto it = reqMethod.find(*id); it != reqMethod.end()) {
      const bool noModel = !reqHasModel[*id];
      const long long bytes = reqBytes.count(*id) != 0u ? reqBytes[*id] : 0;
      probe = it->second == "HEAD" || (noModel && bytes == 0);
    }
    if (probe) {
      probes++;
      continue;
    }
    responses++;
    if (*status < 200 || *status >= 300) {
      errorTurns++;
    }
  }

  std::ostringstream ss;
  ss << "{\"events\":" << lines.size() << ",\"turns\":" << responses
     << ",\"error_turns\":" << errorTurns << ",\"probe_requests\":" << probes
     << ",\"processes\":" << pids.size() << ",\"duration_ms\":" << (tMin < 0 ? 0 : (tMax - tMin))
     << ",\"kinds\":{";
  bool first = true;
  for (const auto& kv : kinds) {
    if (!first) {
      ss << ',';
    }
    first = false;
    ss << '"' << kv.first << "\":" << kv.second;
  }
  ss << "},\"hosts\":{\"tcp\":{";
  first = true;
  for (const auto& kv : tcpHosts) {
    if (!first) {
      ss << ',';
    }
    first = false;
    ss << '"' << kv.first << "\":" << kv.second;
  }
  ss << "},\"unix\":{";
  first = true;
  for (const auto& kv : unixSockets) {
    if (!first) {
      ss << ',';
    }
    first = false;
    ss << '"' << kv.first << "\":" << kv.second;
  }
  ss << "},\"disconnects\":" << disconnects << "},\"files\":{\"written\":" << written.size()
     << ",\"deleted\":" << deleted << ",\"renamed\":" << renamed << "}}";
  return ss.str();
}

void jsonError(httplib::Response& res, int status, const std::string& msg) {
  res.status = status;
  res.set_content("{\"error\":\"" + msg + "\"}", "application/json");
}

bool etagMatch(const httplib::Request& req, const std::string& etag) {
  const std::string inm = req.get_header_value("If-None-Match");
  return !inm.empty() && (inm == etag || inm == "W/" + etag);
}

#endif // SNOWGLOBE_HAS_VIEWER

} // namespace

int cmdView(const std::vector<std::string>& args) {
  std::string run;
  int port = 7777;
  bool open = false;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a.rfind("--port=", 0) == 0) {
      try {
        port = std::stoi(a.substr(7));
      } catch (...) {
        std::cerr << "snowglobe view: bad --port value '" << a << "'\n";
        return kExUsage;
      }
      if (port < 0 || port > 65535) {
        std::cerr << "snowglobe view: port out of range '" << a << "'\n";
        return kExUsage;
      }
    } else if (a == "--port") {
      if (i + 1 >= args.size()) {
        std::cerr << "snowglobe view: --port needs a value\n";
        return kExUsage;
      }
      try {
        port = std::stoi(args[++i]);
      } catch (...) {
        std::cerr << "snowglobe view: bad --port value\n";
        return kExUsage;
      }
    } else if (a == "--open") {
      open = true;
    } else if (a.rfind("--", 0) == 0) {
      std::cerr << "snowglobe view: unknown flag '" << a << "'\n";
      return kExUsage;
    } else if (run.empty()) {
      run = a;
    } else {
      std::cerr << "snowglobe view: unexpected argument '" << a << "'\n";
      return kExUsage;
    }
  }
  if (run.empty()) {
    std::cerr << "usage: snowglobe view <run|events.jsonl> [--port=7777] [--open]\n";
    return kExUsage;
  }
#ifndef SNOWGLOBE_HAS_VIEWER
  (void)port;
  (void)open;
  std::cerr << "snowglobe view: this binary was built without the viewer "
               "(SNOWGLOBE_BUILD_VIEWER=OFF)\n";
  return kExUnavailable;
#else
  namespace fs = std::filesystem;
  RunSource rs;
  std::error_code ec;
  if (fs::is_regular_file(run, ec) && !ec) {
    // Bare events.jsonl: no manifest, no blobs — synthesize the manifest.
    rs.isFile = true;
    rs.eventsPath = fs::absolute(run, ec).string();
  } else if (fs::is_directory(run, ec) && !ec) {
    rs.runDir = fs::canonical(run, ec).string();
    if (ec) {
      std::cerr << "snowglobe view: cannot resolve run dir " << run << "\n";
      return kExSoftware;
    }
    rs.eventsPath = rs.runDir + "/events.jsonl";
    const std::string manifestPath = rs.runDir + "/manifest.json";
    if (!readFile(manifestPath, rs.manifestText)) {
      std::cerr << "snowglobe view: note: no manifest.json, synthesizing one\n";
    }
  } else {
    std::cerr << "snowglobe view: not a run dir or events.jsonl file: " << run << "\n";
    return kExUsage;
  }
  std::string indexErr;
  if (!buildIndex(rs, indexErr)) {
    std::cerr << "snowglobe view: " << indexErr << "\n";
    return kExSoftware;
  }
  if (rs.manifestText.empty()) {
    rs.manifestText = synthesizeManifest(rs.eventsPath, rs.offsets.size());
  }
  const std::string summaryText = buildSummary(rs);
  // Weak ETag for trace payloads (content is static for a finished run).
  const auto traceMtime = fs::last_write_time(rs.eventsPath, ec);
  const auto traceSize = fs::file_size(rs.eventsPath, ec);
  const std::string traceEtag =
      "W/\"" + std::to_string(rs.offsets.size()) + "-" + std::to_string(traceSize) + "\"";
  (void)traceMtime;

  httplib::Server svr;
  // Embedded viewer + trace APIs. /trace/* is the Phase-1C-early path and
  // stays as a compat alias of /api/blob/.
  svr.Get(".*", [&](const httplib::Request& req, httplib::Response& res) {
    std::string p = req.path.empty() ? "/" : req.path;
    // Strip query leftovers defensively (httplib splits params already).
    if (const size_t q = p.find('?'); q != std::string::npos) {
      p = p.substr(0, q);
    }
    if (p == "/") {
      p = "/index.html";
    }
    if (p == "/api/manifest") {
      if (etagMatch(req, traceEtag)) {
        res.status = 304;
        return;
      }
      res.set_header("ETag", traceEtag);
      res.set_header("Cache-Control", "no-store");
      res.set_content(rs.manifestText, "application/json");
      return;
    }
    if (p == "/api/events") {
      long from = 0;
      long to = 0;
      if (!parseBound(req.get_param_value("from"), 0, from) ||
          !parseBound(req.get_param_value("to"), from + kDefaultPage, to)) {
        jsonError(res, 400, "bad from/to (non-negative integers, to > from)");
        return;
      }
      const auto total = static_cast<long>(rs.offsets.size());
      if (to < from) {
        jsonError(res, 400, "bad from/to (non-negative integers, to > from)");
        return;
      }
      if (to - from > kMaxPage) {
        jsonError(res, 400, "page too large (max 5000 events)");
        return;
      }
      if (from > total) {
        jsonError(res, 416, "from beyond end of trace");
        return;
      }
      if (to > total) {
        to = total;
      }
      if (etagMatch(req, traceEtag)) {
        res.status = 304;
        return;
      }
      res.set_header("ETag", traceEtag);
      res.set_header("Cache-Control", "no-store");
      res.set_content(eventsSlice(rs, from, to), "application/json");
      return;
    }
    if (p == "/api/summary") {
      if (etagMatch(req, traceEtag)) {
        res.status = 304;
        return;
      }
      res.set_header("ETag", traceEtag);
      res.set_header("Cache-Control", "no-store");
      res.set_content(summaryText, "application/json");
      return;
    }
    if (startsWith(p, "/api/blob/")) {
      const std::string rel = p.substr(10);
      std::string abs;
      if (!resolveBlob(rs, rel, abs)) {
        res.status = 404;
        res.set_content("not found", "text/plain");
        return;
      }
      std::string body;
      if (!readFile(abs, body)) {
        res.status = 404;
        res.set_content("not found", "text/plain");
        return;
      }
      res.set_header("ETag", traceEtag);
      res.set_header("Cache-Control", "no-cache");
      res.set_content(body, mimeFor(rel));
      return;
    }
    if (startsWith(p, "/trace/")) {
      const std::string rel = p.substr(7);
      std::string abs;
      if (!resolveBlob(rs, rel, abs)) {
        res.status = 404;
        res.set_content("not found", "text/plain");
        return;
      }
      std::string body;
      if (!readFile(abs, body)) {
        res.status = 404;
        res.set_content("not found", "text/plain");
        return;
      }
      res.set_header("ETag", traceEtag);
      res.set_header("Cache-Control", "no-cache");
      res.set_content(body, mimeFor(rel));
      return;
    }
    for (unsigned long i = 0; i < viewer_data::kFileCount; ++i) {
      const auto& f = viewer_data::kFiles[i];
      if (p == f.path) {
        if (etagMatch(req, f.etag)) {
          res.status = 304;
          return;
        }
        res.set_header("ETag", f.etag);
        if (f.immutable) {
          res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        } else {
          res.set_header("Cache-Control", "no-cache");
        }
        res.set_header("X-Content-Type-Options", "nosniff");
        std::string body(reinterpret_cast<const char*>(f.data), f.size);
        if (f.gzipped) {
          // Stored gzip-compressed at build time (see
          // cmake/embed_viewer.cmake). Browsers always send
          // Accept-Encoding: gzip; curl users pass --compressed.
          res.set_header("Content-Encoding", "gzip");
        }
        res.set_content(body, f.mime);
        return;
      }
    }
    res.status = 404;
    res.set_content("not found", "text/plain");
  });

  int bound = -1;
  if (port != 0 && svr.bind_to_port("127.0.0.1", port)) {
    bound = port;
  } else {
    bound = svr.bind_to_any_port("127.0.0.1");
    if (bound > 0 && port != 0) {
      std::cerr << "snowglobe view: port " << port << " busy, using " << bound << "\n";
    }
  }
  if (bound <= 0) {
    std::cerr << "snowglobe view: cannot bind 127.0.0.1 (try --port=N)\n";
    return kExSoftware;
  }
  const std::string url = "http://127.0.0.1:" + std::to_string(bound);
  std::cerr << "view: " << url << " " << run << " (" << rs.offsets.size() << " events)\n";
#ifndef _WIN32
  if (open) {
    // Never fail without xdg-open: degrade to the printed URL.
    if (haveXdgOpen()) {
      launchBrowser(url);
    } else {
      std::cerr << "snowglobe view: --open: xdg-open not found, serving only\n";
    }
  }
#else
  (void)open;
#endif
  svr.listen_after_bind(); // foreground until SIGINT/SIGTERM
  return 0;
#endif
}

} // namespace snowglobe::view

// Format-agnostic LLM recording proxy (Phase 1B Block 1).
//
// Design (why no ADR-0004 epoll forwarder): cpp-httplib CAN stream both
// directions the way we need — server-side ContentReader (request receive),
// client Request::content_receiver (response download, the same hook the
// Get-with-receiver overloads use), server chunked content providers
// (response send). Uploads are buffered per request (bounded by client
// behavior; 50 MB transient worst case) while downloads stream
// chunk-by-chunk with zero application buffering. A bounded BlockingQueue
// bridges the push-model upstream receiver and the pull-model server
// provider.
#include "proxy.hpp"

#include "../redact/redact.hpp"
#include "../util/string_util.hpp"
#include "base64.hpp"
#include "blocking_queue.hpp"
#include "model_scan.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace snowglobe::proxy {
namespace {

uint64_t nowUs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

uint64_t nowMonoMs() {
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

pid_t thisTid() {
  return static_cast<pid_t>(syscall(SYS_gettid));
}

bool startsWith(const std::string& s, const std::string& pre) {
  return s.compare(0, pre.size(), pre) == 0;
}

std::string lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower((unsigned char)c));
  }
  return s;
}

// Hop-by-hop headers: consumed here, never forwarded upstream. Everything
// else (including Authorization — end-to-end) forwards byte-identical;
// storage redacts separately (ADR-0003). httplib also injects
// remote_addr/remote_port/local_addr/local_port into req.headers (verified:
// plain curl sends none of them); those describe the downstream hop and are
// stripped from both paths.
bool isHopByHop(const std::string& name) {
  const std::string n = lower(name);
  return n == "connection" || n == "keep-alive" || n == "proxy-authenticate" ||
         n == "proxy-authorization" || n == "te" || n == "trailer" || n == "transfer-encoding" ||
         n == "upgrade" || n == "host" || n == "content-length" || n == "remote_addr" ||
         n == "remote_port" || n == "local_addr" || n == "local_port";
}

std::string resExt(const std::string& contentType) {
  const std::string ct = lower(contentType);
  if (ct.find("text/event-stream") != std::string::npos) {
    return ".sse";
  }
  if (ct.find("json") != std::string::npos) {
    return ".json";
  }
  return ".bin";
}

std::string padId(long id) {
  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "%04ld", id);
  return buf;
}

std::string stripTrailingSlash(std::string s) {
  while (s.size() > 1 && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

} // namespace

LlmProxy::LlmProxy(ProxyOptions opts) : opts_(std::move(opts)) {}

LlmProxy::~LlmProxy() {
  stop(10);
  if (stopThread_.joinable()) {
    stopThread_.join(); // waits out pathological stuck reads (<= one idle timeout)
  }
}

uint64_t LlmProxy::tMs() const {
  const uint64_t base = tEpochMonoMs_ != 0 ? tEpochMonoMs_ : 0;
  const uint64_t now = nowMonoMs();
  return base != 0 && now >= base ? now - base : 0;
}

void LlmProxy::abandonFlight(Flight& flight, bool closeFiles) {
  if (closeFiles) {
    flight.resFile.close();
    flight.idxFile.close();
  }
  ::unlink((flight.resPath + ".tmp").c_str());
  ::unlink((flight.idxPath + ".tmp").c_str());
}

void LlmProxy::emitRequestEvent(long id, const std::string& provider, const std::string& method,
                                const std::string& path, const JsonTop& jt, uint64_t bytes,
                                uint64_t tsUs, pid_t tid) {
  using util::jsonEscape;
  if (!opts_.sink) {
    return;
  }
  opts_.sink("{\"ts_us\":" + std::to_string(tsUs) + ",\"t_ms\":" + std::to_string(tMs()) +
             ",\"pid\":" + std::to_string(::getpid()) + ",\"tid\":" + std::to_string(tid) +
             ",\"ev\":\"llm.request\",\"id\":" + std::to_string(id) +
             ",\"provider\":" + jsonEscape(provider) + ",\"method\":" + jsonEscape(method) +
             ",\"path\":" + jsonEscape(path) + ",\"model\":" +
             (jt.hasModel ? jsonEscape(jt.model) : "null") + ",\"bytes\":" + std::to_string(bytes) +
             ",\"stream\":" + (jt.hasStream && jt.stream ? "true" : "false") + "}");
}

void LlmProxy::emitResponseEvent(long id, int status, uint64_t bytes, bool hasTtfb, uint64_t ttfbMs,
                                 uint64_t totalMs, uint64_t chunks, bool truncated,
                                 const std::string& reqRel, const std::string& resRel,
                                 const std::string& idxRel, uint64_t tsUs, pid_t tid) {
  using util::jsonEscape;
  if (!opts_.sink) {
    return;
  }
  turns_.fetch_add(1);
  if (status < 200 || status >= 300) {
    errors_.fetch_add(1);
  }
  opts_.sink("{\"ts_us\":" + std::to_string(tsUs) + ",\"t_ms\":" + std::to_string(tMs()) +
             ",\"pid\":" + std::to_string(::getpid()) + ",\"tid\":" + std::to_string(tid) +
             ",\"ev\":\"llm.response\",\"id\":" + std::to_string(id) +
             ",\"status\":" + std::to_string(status) + ",\"bytes\":" + std::to_string(bytes) +
             ",\"ttfb_ms\":" + (hasTtfb ? std::to_string(ttfbMs) : "null") + ",\"total_ms\":" +
             std::to_string(totalMs) + ",\"chunk_count\":" + std::to_string(chunks) +
             ",\"truncated\":" + (truncated ? "true" : "false") + ",\"req\":" + jsonEscape(reqRel) +
             ",\"res\":" + jsonEscape(resRel) + ",\"idx\":" + jsonEscape(idxRel) + "}");
}

LlmProxy::Route LlmProxy::resolve(const std::string& target) const {
  Route r;
  std::string path = target;
  std::string query;
  const size_t q = target.find('?');
  if (q != std::string::npos) {
    path = target.substr(0, q);
    query = target.substr(q); // incl. '?', forwarded byte-exact
  }
  auto finish = [&](const std::string& provider, const std::string& base,
                    const std::string& remainder) {
    r.ok = true;
    r.provider = provider;
    r.base = stripTrailingSlash(base);
    r.path = (remainder.empty() ? "/" : remainder) + query;
  };
  auto overrideOf = [&](const std::string& provider, const std::string& dflt) {
    const auto it = opts_.upstream.find(provider);
    return it != opts_.upstream.end() ? it->second : dflt;
  };
  if (startsWith(path, "/openai/")) {
    finish("openai", overrideOf("openai", "https://api.openai.com"), path.substr(7));
    return r;
  }
  if (path == "/openai") {
    finish("openai", overrideOf("openai", "https://api.openai.com"), "/");
    return r;
  }
  if (startsWith(path, "/anthropic/")) {
    finish("anthropic", overrideOf("anthropic", "https://api.anthropic.com"), path.substr(10));
    return r;
  }
  if (path == "/anthropic") {
    finish("anthropic", overrideOf("anthropic", "https://api.anthropic.com"), "/");
    return r;
  }
  if (startsWith(path, "/gemini/")) {
    finish("gemini", overrideOf("gemini", "https://generativelanguage.googleapis.com"),
           path.substr(7));
    return r;
  }
  if (path == "/gemini") {
    finish("gemini", overrideOf("gemini", "https://generativelanguage.googleapis.com"), "/");
    return r;
  }
  if (startsWith(path, "/u/")) {
    const std::string rest = path.substr(3);
    const size_t slash = rest.find('/');
    const std::string b64 = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    const std::string remainder = (slash == std::string::npos) ? "/" : rest.substr(slash);
    std::string base;
    if (!base64Decode(b64, base, true)) {
      return r;
    }
    if (!startsWith(base, "http://") && !startsWith(base, "https://")) {
      return r; // never proxy to non-HTTP schemes
    }
    finish("custom", base, remainder);
    return r;
  }
  return r; // ok=false: caller records a 404
}

int LlmProxy::start() {
  tEpochMonoMs_ = nowMonoMs();
  svr_.set_read_timeout(130, 0);
  svr_.set_write_timeout(130, 0);
  svr_.set_keep_alive_timeout(5);
  svr_.new_task_queue = [] { return new httplib::ThreadPool(32); };
  // NOTE: ThreadPool(32), not thread-per-connection: bounded resources with
  // explicit headroom for burst concurrency; excess connections wait in the
  // listen backlog. All Block 2 concurrency targets (8 streams) fit.

  svr_.Get(".*",
           [this](const httplib::Request& req, httplib::Response& res) { handlePlain(req, res); });
  auto bodyHandler = [this](const httplib::Request& req, httplib::Response& res,
                            const httplib::ContentReader& reader) { handleBody(req, res, reader); };
  svr_.Post(".*", bodyHandler);
  svr_.Put(".*", bodyHandler);
  svr_.Patch(".*", bodyHandler);
  svr_.Delete(".*", bodyHandler);

  port_ = svr_.bind_to_any_port("127.0.0.1");
  if (port_ <= 0) {
    error_ = "bind 127.0.0.1:<ephemeral> failed";
    return -1;
  }
  serveThread_ = std::thread([this] { svr_.listen_after_bind(); });
  // listen_after_bind runs async: poll with bare TCP connects until one
  // succeeds. Raw connects (no HTTP) never reach a handler, so readiness
  // leaves zero trace in the recording.
  for (int i = 0; i < 1000; ++i) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    bool ready = false;
    if (fd >= 0) {
      struct sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(port_));
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      ready = ::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
      ::close(fd);
    }
    if (ready) {
      break;
    }
    usleep(5000);
    if (i == 999) {
      error_ = "proxy socket never became ready";
      return -1;
    }
  }
  return port_;
}

void LlmProxy::stop(long deadlineS) {
  {
    std::lock_guard<std::mutex> lk(stopMu_);
    if (stopStarted_) {
      return;
    }
    stopStarted_ = true;
  }
  shuttingDown_.store(true); // in-flight handlers abort per-chunk promptly
  stopThread_ = std::thread([this] { svr_.stop(); });
  const uint64_t end = nowMonoMs() + static_cast<uint64_t>(deadlineS) * 1000ULL;
  for (;;) {
    size_t live = 0;
    {
      std::lock_guard<std::mutex> lk(liveMu_);
      live = live_.size();
    }
    if (live == 0 || nowMonoMs() >= end) {
      break;
    }
    usleep(5000);
  }
  // Past the deadline with flights still live: abort them all (prompt
  // provider abort; upstream receiver aborts; threads unwind and are joined
  // by their providers) so shutdown never hangs on one stuck stream.
  {
    std::lock_guard<std::mutex> lk(liveMu_);
    for (const auto& f : live_) {
      f->abort.store(true);
      f->respQ.close();
    }
  }
  // Join everything: stopThread_ (svr_.stop, prompt once handlers unwind),
  // then serveThread_ (listen loop ends when stopped). Worst case is one
  // idle timeout for a read that stalled exactly across shutdown; the common
  // case returns in milliseconds. Never detach, never terminate.
  if (stopThread_.joinable()) {
    stopThread_.join();
  }
  if (serveThread_.joinable()) {
    serveThread_.join();
  }
}

void LlmProxy::handlePlain(const httplib::Request& req, httplib::Response& res) {
  // GET-style: no ContentReader form exists; bodies (if any) arrive buffered
  // and LLM GETs carry none. Unify by materializing the body string.
  httplib::ContentReader reader(
      [&](httplib::ContentReceiver receiver) {
        if (!req.body.empty()) {
          return receiver(req.body.data(), req.body.size());
        }
        return true;
      },
      [](httplib::MultipartContentHeader, httplib::ContentReceiver) { return false; });
  handleBody(req, res, reader);
}

void LlmProxy::handleBody(const httplib::Request& req, httplib::Response& res,
                          const httplib::ContentReader& reader) {
  using util::jsonEscape;
  const pid_t tid = thisTid();
  const uint64_t tStartWall = nowUs();
  const uint64_t tStartMono = nowMonoMs();

  const Route rt = resolve(req.target);
  const long id = nextId_.fetch_add(1);
  const std::string num = padId(id);
  const std::string llmDir = opts_.runDir + "/llm";
  const std::string reqRel = "llm/" + num + ".req.json";

  // Request body (buffered per request; bounded by client behavior).
  // A vanished uploader yields no request at all: nothing recordable.
  std::string body;
  const bool readOk = reader([&](const char* d, size_t n) {
    body.append(d, n);
    return true;
  });
  if (!readOk) {
    return;
  }

  // Stored request envelope: headers redacted (ADR-0003), body verbatim
  // (base64-wrapped when not UTF-8 so the file stays valid JSON). Bodies
  // are NOT token-redacted (replay needs them verbatim; share is gated).
  const std::string provider = rt.ok ? rt.provider : "unknown";
  {
    std::ofstream f(llmDir + "/" + num + ".req.json", std::ios::trunc | std::ios::binary);
    f << "{\"method\":" << jsonEscape(req.method) << ",\"path\":" << jsonEscape(req.path)
      << ",\"provider\":" << jsonEscape(provider) << ",\"headers\":{";
    bool first = true;
    for (const auto& [k, v] : req.headers) {
      if (isHopByHop(k)) {
        continue; // framing + server-injected metadata are not request content
      }
      if (!first) {
        f << ",";
      }
      first = false;
      f << jsonEscape(k) << ":" << jsonEscape(redact::redactHeaderValue(k, v));
    }
    const bool utf8 = isUtf8(body);
    f << "},\"body\":" << (utf8 ? jsonEscape(body) : jsonEscape(base64Encode(body)))
      << ",\"body_encoding\":" << (utf8 ? "\"utf8\"" : "\"base64\"") << "}";
  }

  const JsonTop jt = scanJsonTop(body);
  emitRequestEvent(id, provider, req.method, req.path, jt, body.size(), tStartWall, tid);

  if (!rt.ok) {
    // Unknown route: recorded like any response (debugging agents that hit
    // wrong URLs), synthesized 404, uniform blob layout.
    const std::string resBody = "{\"error\":\"unknown snowglobe route\"}";
    {
      std::ofstream f(llmDir + "/" + num + ".res.json", std::ios::trunc | std::ios::binary);
      f << resBody;
    }
    { std::ofstream f(llmDir + "/" + num + ".res.idx", std::ios::trunc); }
    emitResponseEvent(id, 404, resBody.size(), false, 0, nowMonoMs() - tStartMono, 0, false, reqRel,
                      "llm/" + num + ".res.json", "llm/" + num + ".res.idx", nowUs(), tid);
    res.status = 404;
    res.set_content(resBody, "application/json");
    return;
  }

  // Forwarded headers: everything except hop-by-hop (Host/Content-Length are
  // recomputed by httplib from the client target). Authorization and friends
  // forward byte-identical; only the STORED copy is redacted.
  httplib::Headers fwd;
  for (const auto& [k, v] : req.headers) {
    if (isHopByHop(k)) {
      continue;
    }
    fwd.emplace(k, v);
  }
  const std::string method = req.method;

  auto flight = std::shared_ptr<Flight>(new Flight(), [](Flight* f) { delete f; });
  flight->tStartWall = tStartWall;
  flight->tStartMono = tStartMono;
  flight->resPath = llmDir + "/" + num + ".res";
  flight->idxPath = llmDir + "/" + num + ".res.idx";
  {
    std::lock_guard<std::mutex> lk(liveMu_);
    live_.insert(flight);
  }
  auto unlive = [&] {
    std::lock_guard<std::mutex> lk(liveMu_);
    live_.erase(flight);
  };
  auto joinUp = [&] {
    if (flight->upThread.joinable()) {
      flight->upThread.join();
    }
  };

  // Upstream worker: one client per request (no shared-client races), send
  // with buffered body + streaming receiver. try/catch: an escaping
  // exception would terminate via the thread. Timeouts are snapshotted by
  // value (never `this->opts_`): the shutdown-during-head-wait path below
  // detaches this thread, so it must not touch the LlmProxy object, which
  // may be destroyed while a stuck upstream connect/read is still pending.
  const long connectTimeoutS = opts_.connectTimeoutS;
  const long idleTimeoutS = opts_.idleTimeoutS;
  flight->upThread = std::thread(
      [flight, rt, fwd, method, body = std::move(body), connectTimeoutS, idleTimeoutS]() mutable {
        try {
          httplib::Client cli(rt.base);
          cli.set_connection_timeout(connectTimeoutS, 0);
          cli.set_read_timeout(idleTimeoutS, 0); // idle-between-bytes semantics
          cli.set_write_timeout(idleTimeoutS, 0);
          cli.set_decompress(false); // byte-exact bodies, never transcoded
          // TLS verification stays ON (httplib default); the system CA bundle
          // honors SSL_CERT_FILE/SSL_CERT_DIR via OpenSSL defaults.

          httplib::Request ureq;
          // NOTE: httplib has no public POST-with-provider AND receiver combo;
          // Request+send() with req.body set uploads buffered while
          // req.content_receiver streams the download — the same hook the
          // Get-with-receiver overloads use internally.
          ureq.method = method;
          ureq.path = rt.path;
          ureq.headers = fwd;
          ureq.body = std::move(body);
          ureq.response_handler = [flight](const httplib::Response& r) {
            std::lock_guard<std::mutex> lk(flight->mu);
            flight->status = r.status;
            flight->resHeaders = r.headers;
            flight->resContentType = r.get_header_value("Content-Type", "");
            flight->resExt = resExt(flight->resContentType);
            // NOTE: ttfb is first BODY byte (set in content_receiver), not head
            // arrival — head time is request RTT, useless for streaming latency.
            flight->headDone = true;
            flight->cv.notify_all();
            return !flight->abort.load();
          };
          ureq.content_receiver = [flight](const char* d, size_t n, uint64_t, uint64_t) {
            if (flight->abort.load()) {
              return false;
            }
            if (!flight->haveTtfb) {
              flight->haveTtfb = true;
              flight->ttfbWall = nowUs();
            }
            char line[96] = {};
            std::snprintf(line, sizeof(line), "{\"off\":%llu,\"ts_us\":%llu}",
                          (unsigned long long)flight->bytes, (unsigned long long)nowUs());
            flight->idxFile << line << "\n";
            flight->resFile.write(d, static_cast<std::streamsize>(n));
            flight->bytes += n;
            ++flight->chunks;
            flight->respQ.push(std::string(d, n));
            return !flight->abort.load();
          };

          // Open blob files before dispatch (a crash mid-stream keeps the prefix).
          flight->resFile.open(flight->resPath + ".tmp", std::ios::trunc | std::ios::binary);
          flight->idxFile.open(flight->idxPath + ".tmp", std::ios::trunc);
          httplib::Response ures;
          httplib::Error err = httplib::Error::Success;
          const bool ok = cli.send(ureq, ures, err);
          {
            std::lock_guard<std::mutex> lk(flight->mu);
            if (!flight->headDone) {
              flight->headError = true;
              flight->headErrText = ok ? "empty upstream response" : httplib::to_string(err);
              flight->headDone = true;
            } else if (!ok && !flight->abort.load()) {
              flight->upError = true; // broke mid-stream with a complete head
            }
            flight->cv.notify_all();
          }
          flight->respQ.close();
          if (flight->detached.load()) {
            abandonFlight(*flight, true);
          }
        } catch (...) {
          std::lock_guard<std::mutex> lk(flight->mu);
          if (!flight->headDone) {
            flight->headError = true;
            flight->headErrText = "upstream exception";
            flight->headDone = true;
          }
          flight->cv.notify_all();
          flight->respQ.close();
          if (flight->detached.load()) {
            abandonFlight(*flight, true);
          }
        }
      });

  // Wait for the upstream head (no timeout: LLM TTFB is unbounded; a hung
  // upstream surfaces via the idle-between-bytes read timeout as an error).
  // Shutdown during the wait abandons the flight: detach (never join a
  // possibly-stuck connect here — the deadline is real), record nothing.
  {
    std::unique_lock<std::mutex> lk(flight->mu);
    flight->cv.wait(lk, [&] { return flight->headDone || shuttingDown_.load(); });
    if (!flight->headDone) {
      lk.unlock();
      flight->abort.store(true);
      flight->respQ.close();
      // Detach: shared_ptr keeps Flight alive; the thread unwinds alone and
      // removes its own .tmps on completion (abandonFlight). Joins happen
      // only on paths where the worker already finished.
      flight->detached.store(true);
      if (flight->upThread.joinable()) {
        flight->upThread.detach();
      }
      // Unlink-only here (the worker may still be writing): the thread-end
      // close+unlink is idempotent, so every interleaving ends tmp-free.
      abandonFlight(*flight, false);
      unlive();
      return;
    }
  }

  if (flight->headError) {
    // Upstream unreachable: synthesized 502 carrying the error as its body.
    // The .tmps were opened before dispatch but this path writes finals
    // directly, so remove the orphans (a real Claude Code run left
    // 0000.res.tmp + 0000.res.idx.tmp behind here).
    joinUp();
    abandonFlight(*flight, true);
    const std::string errBody =
        "{\"error\":\"upstream failed\",\"detail\":" + util::jsonEscape(flight->headErrText) + "}";
    const std::string resRel = "llm/" + num + ".res.json";
    const std::string idxRel = "llm/" + num + ".res.idx";
    {
      std::ofstream f(llmDir + "/" + num + ".res.json", std::ios::trunc | std::ios::binary);
      f << errBody;
    }
    { std::ofstream f(llmDir + "/" + num + ".res.idx", std::ios::trunc); }
    emitResponseEvent(id, 502, errBody.size(), false, 0, nowMonoMs() - tStartMono, 0, false, reqRel,
                      resRel, idxRel, nowUs(), tid);
    res.status = 502;
    res.set_content(errBody, "application/json");
    unlive();
    return;
  }

  // Copy head state under lock (write happened under mu in the worker).
  httplib::Headers resHeaders;
  int status = 0;
  {
    std::lock_guard<std::mutex> lk(flight->mu);
    resHeaders = flight->resHeaders;
    status = flight->status;
  }
  for (const auto& [k, v] : resHeaders) {
    // Content-Type is set by set_chunked_content_provider below (Headers is
    // a multimap: set_header would duplicate it — observed on the wire).
    if (!isHopByHop(k) && lower(k) != "content-length" && lower(k) != "content-type") {
      res.set_header(k, v);
    }
  }
  res.status = status;
  auto provFlight = flight;
  // Rename .tmp blobs to final names once the extension is known. The rename
  // happens after the upstream thread joined (files complete).
  res.set_chunked_content_provider(
      flight->resContentType.empty() ? "application/octet-stream" : flight->resContentType,
      [this, provFlight, id, reqRel, num, tid](size_t, httplib::DataSink& sink) {
        std::string chunk;
        while (provFlight->respQ.pop(chunk)) {
          if (!sink.write(chunk.data(), chunk.size())) {
            // Client gone mid-stream: abort upstream, keep the partial.
            provFlight->abort.store(true);
            provFlight->respQ.close();
            break;
          }
        }
        if (provFlight->upThread.joinable()) {
          provFlight->upThread.join();
        }
        provFlight->resFile.close();
        provFlight->idxFile.close();
        ::rename((provFlight->resPath + ".tmp").c_str(),
                 (provFlight->resPath + provFlight->resExt).c_str());
        ::rename((provFlight->idxPath + ".tmp").c_str(), provFlight->idxPath.c_str());
        const uint64_t total = nowMonoMs() - provFlight->tStartMono;
        // Empty clean body (e.g. 204, error pages with no chunks) is not
        // truncated — only abort/upstream-error/never-any-byte is.
        const bool reallyTrunc = provFlight->abort.load() || provFlight->upError ||
                                 (!provFlight->haveTtfb && provFlight->bytes > 0);
        emitResponseEvent(
            id, provFlight->status, provFlight->bytes, provFlight->haveTtfb,
            provFlight->haveTtfb ? (provFlight->ttfbWall - provFlight->tStartWall + 500) / 1000 : 0,
            total, provFlight->chunks, reallyTrunc, reqRel,
            "llm/" + num + ".res" + provFlight->resExt, "llm/" + num + ".res.idx", nowUs(), tid);
        {
          std::lock_guard<std::mutex> lk(liveMu_);
          live_.erase(provFlight);
        }
        sink.done();  // zero-chunk terminator; without it clients see EOF
                      // mid-chunked-body (curl exit 18). No-op on dead sockets.
        return false; // end of stream
      });
}

} // namespace snowglobe::proxy

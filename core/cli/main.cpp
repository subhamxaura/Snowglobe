// snowglobe CLI — run (tracer + LLM proxy) + view (embedded viewer) +
// doctor + version + ls/rm stubs.
// Human output → stderr; machine output (--json) → stdout (AGENTS.md §2).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "../doctor/doctor.hpp"
#include "../isolate/isolate.hpp"
#include "../link/cli.hpp"
#include "../proxy/proxy.hpp"
#include "../redact/redact.hpp"
#include "../trace/jsonl_writer.hpp"
#include "../tracer/common/itracer.hpp"
#include "../tracer/ptrace/ptrace_tracer.hpp"
#include "../util/sha256.hpp"
#include "../util/string_util.hpp"
#include "../version.hpp"
#include "../view/view.hpp"

namespace fs = std::filesystem;
using snowglobe::util::jsonEscape;

// Global environ (declared here: <unistd.h> may not expose it under -Wpedantic).
extern char** environ; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

namespace {

constexpr int kExUsage = 64;
constexpr int kExUnavailable = 69;
constexpr int kExSoftware = 70;

void usage(std::ostream& os) {
  os << "Usage:\n"
     << "  snowglobe run [--project=DIR] [--out=DIR] [--tracer=auto|ptrace] [-a|--all-opens]\n"
     << "                [--capture-stdio] [--json] [--no-llm-proxy] [--isolate]\n"
     << "                [--fs-rw=PATH]... [--allow-env=NAME]... [--allow-path=PATH]...\n"
     << "                [--memory-max=SIZE] [--pids-max=N]\n"
     << "                [--upstream=PROVIDER=URL]... -- <command> [args...]\n"
     << "  snowglobe doctor\n"
     << "  snowglobe version [--json]\n"
     << "  snowglobe ls [--json]\n"
     << "  snowglobe rm <run>\n"
     << "  snowglobe view <run> [--port=7777] [--open]\n"
     << "  snowglobe link <run> [--check]\n"
     << "  snowglobe diff|apply|replay|compare|share <run> ... (not yet implemented)\n";
}

std::string rfc3339Utc(std::time_t t) {
  char buf[32] = {};
  std::tm tmv = {};
#ifdef _WIN32
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return buf;
}

std::string runTimestamp() {
  const auto now = std::chrono::system_clock::now();
  return rfc3339Utc(std::chrono::system_clock::to_time_t(now));
}

std::string compactTs() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  char buf[32] = {};
  std::tm tmv = {};
#ifdef _WIN32
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tmv);
  return buf;
}

std::string randSuffix() {
  static const char* kChars = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::random_device rd;
  std::string out;
  for (int i = 0; i < 6; ++i) {
    out.push_back(kChars[rd() % 36]);
  }
  return out;
}

std::string kernelStr() {
#ifdef __linux__
  struct utsname u = {};
  if (uname(&u) == 0) {
    return std::string(u.sysname) + " " + u.release + " " + u.machine;
  }
  return "Linux unknown";
#else
  return "non-Linux (dev box)";
#endif
}

std::string cwdStr() {
  std::error_code ec;
  const auto p = fs::current_path(ec);
  if (!ec) {
    return p.string();
  }
  return "";
}

// Fingerprint = sha256 of sorted env var NAMES only (never values).
std::string envFingerprint() {
  std::vector<std::string> names;
#ifndef _WIN32
  for (char** e = ::environ; e != nullptr && *e != nullptr; ++e) {
    std::string s(*e);
    names.push_back(s.substr(0, s.find('=')));
  }
#else
  // Windows dev box: names unavailable without CRT internals; hash a constant
  // so manifests stay well-formed (CI/Linux takes the real path).
  names.push_back("windows-dev-box");
#endif
  std::sort(names.begin(), names.end());
  std::string joined;
  for (const auto& n : names) {
    joined += n;
    joined += '\n';
  }
  return snowglobe::util::sha256Hex(joined);
}

fs::path runsRoot() {
  if (const char* home = std::getenv("SNOWGLOBE_HOME"); home != nullptr && home[0] != '\0') {
    return fs::path(home) / "runs";
  }
  return fs::path(".snowglobe") / "runs";
}

struct RunOptions {
  std::string project;
  std::string out;
  std::string tracer = "auto";
  bool allOpens = false;
  bool captureStdio = false;
  bool json = false;
  bool noLlmProxy = false;
  bool isolate = false;                      // --isolate: unprivileged userns + overlay (ADR-0007)
  std::vector<std::string> isolateFsRw;      // --fs-rw PATH (repeatable, isolate only)
  std::vector<std::string> isolateAllowEnv;  // --allow-env NAME (repeatable, isolate only)
  std::vector<std::string> isolateAllowPath; // --allow-path PATH (repeatable, isolate only)
  std::string isolateMemMax;                 // --memory-max (isolate only)
  std::string isolatePidsMax;                // --pids-max (isolate only)
  std::vector<std::string> upstreams;        // raw PROVIDER=URL strings
  std::vector<std::string> cmd;
};

int cmdRun(const RunOptions& o) {
  if (o.cmd.empty()) {
    std::cerr << "snowglobe run: missing command after --\n";
    return kExUsage;
  }
  const std::string tracerName = o.tracer == "auto" ? "ptrace" : o.tracer;
  if (tracerName != "ptrace") {
    std::cerr << "snowglobe run: tracer '" + o.tracer + "' unavailable in v0.1 (only ptrace)\n";
    return kExUnavailable;
  }

  fs::path runDir;
  if (!o.out.empty()) {
    runDir = fs::path(o.out);
  } else {
    runDir = runsRoot() / (compactTs() + "-" + randSuffix() + ".sgr");
  }
  std::error_code ec;
  fs::create_directories(runDir, ec);
  if (ec) {
    std::cerr << "snowglobe run: cannot create " << runDir.string() << ": " << ec.message() << "\n";
    return kExSoftware;
  }

  const std::string started = runTimestamp();
  const std::string cwd = cwdStr();
  const std::string project = o.project.empty() ? cwd : o.project;

  // --isolate prep (ADR-0007): absolute overlay backing dirs under the run
  // dir; the project must exist and cannot be / itself. The isolate-only
  // flags without --isolate are usage errors (loud, never silently idle).
  if (!o.isolate &&
      (!o.isolateFsRw.empty() || !o.isolateAllowEnv.empty() || !o.isolateAllowPath.empty() ||
       !o.isolateMemMax.empty() || !o.isolatePidsMax.empty())) {
    std::cerr << "snowglobe run: --fs-rw/--allow-env/--allow-path/--memory-max/--pids-max need "
                 "--isolate\n";
    return kExUsage;
  }
  snowglobe::isolate::OverlayDirs isoDirs;
  std::vector<snowglobe::isolate::FsRwMount> isoFsRw;
  std::string isoProjectAbs;
  std::string isoHomeAbs;
  std::vector<std::string> isoAllowAbs;
  std::vector<std::string> isoMasks;
  long long isoMemBytes = -1;
  bool isoMemExplicit = false;
  long long isoPidsMax = 512;
  if (o.isolate) {
#ifdef __linux__
    std::error_code pec;
    const fs::path pabs = fs::absolute(fs::path(project), pec);
    if (pec || !fs::is_directory(pabs, pec) || pec) {
      std::cerr << "snowglobe run: --isolate needs an existing --project dir, got '" << project
                << "'\n";
      return kExUsage;
    }
    isoProjectAbs = pabs.lexically_normal().string();
    if (isoProjectAbs == "/") {
      std::cerr << "snowglobe run: --isolate cannot take / as the project\n";
      return kExUsage;
    }
    for (const char* special : {"/proc", "/sys", "/dev"}) {
      const std::string s = special;
      if (isoProjectAbs == s ||
          (isoProjectAbs.size() > s.size() && isoProjectAbs.compare(0, s.size(), s) == 0 &&
           isoProjectAbs[s.size()] == '/')) {
        std::cerr << "snowglobe run: --isolate cannot take " << isoProjectAbs
                  << " as the project (special filesystem)\n";
        return kExUsage;
      }
    }
    // $HOME is overlaid writable (agents keep state there); missing HOME
    // fails loud rather than running half-contained.
    const char* home = ::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
      std::cerr << "snowglobe run: --isolate needs $HOME set\n";
      return kExUsage;
    }
    {
      std::error_code hec;
      const fs::path habs = fs::absolute(fs::path(home), hec);
      if (hec || !fs::is_directory(habs, hec) || hec) {
        std::cerr << "snowglobe run: --isolate needs an existing $HOME dir, got '" << home << "'\n";
        return kExUsage;
      }
      isoHomeAbs = habs.lexically_normal().string();
    }
    // --fs-rw targets: absolute existing dirs (files cannot overlay).
    std::vector<std::string> fsRwAbs;
    for (const std::string& r : o.isolateFsRw) {
      std::error_code rec2;
      const fs::path rabs = fs::absolute(fs::path(r), rec2);
      if (rec2 || !fs::is_directory(rabs, rec2) || rec2) {
        std::cerr << "snowglobe run: --fs-rw needs an existing dir, got '" << r << "'\n";
        return kExUsage;
      }
      fsRwAbs.push_back(rabs.lexically_normal().string());
    }
    // --allow-path exemptions: absolute paths (need not exist — secret dirs
    // like ~/.ssh may be absent; exact match or parent exempts the mask).
    // ssh-based git remotes need --allow-path for ~/.ssh (documented).
    isoAllowAbs.clear();
    for (const std::string& r : o.isolateAllowPath) {
      std::error_code rec3;
      const fs::path rabs = fs::absolute(fs::path(r), rec3);
      if (rec3) {
        std::cerr << "snowglobe run: --allow-path needs an absolute path, got '" << r << "'\n";
        return kExUsage;
      }
      const std::string s = rabs.lexically_normal().string();
      if (s.empty() || s[0] != '/') {
        std::cerr << "snowglobe run: --allow-path needs an absolute path, got '" << r << "'\n";
        return kExUsage;
      }
      isoAllowAbs.push_back(s);
    }
    // Parse --memory-max/--pids-max NOW (before spawnMiddle): the child
    // needs them for the prlimit fallback (NPROC+NOFILE always, AS only
    // when --memory-max was explicit — AS breaks Bun).
    isoMemBytes = -1;
    isoMemExplicit = false;
    isoPidsMax = 512;
    if (!o.isolateMemMax.empty()) {
      std::string perr;
      if (!snowglobe::isolate::parseMemSize(o.isolateMemMax, isoMemBytes, perr)) {
        std::cerr << "snowglobe run: --memory-max " << perr << "\n";
        return kExUsage;
      }
      isoMemExplicit = true;
    }
    if (!o.isolatePidsMax.empty()) {
      if (o.isolatePidsMax == "max") {
        isoPidsMax = -1;
        // -1 means no NPROC limit? prlimit RLIMIT_NPROC -1 = unlimited?
        // For safety, treat "max" as no prlimit NPROC change (skip)? No:
        // keep simple: -1 passes through to cgroup (no limit) and to
        // setrlimit as RLIM_INFINITY. setrlimit handles -1? RLIM_INFINITY
        // is ~0ULL, not -1. Handle below: if -1, skip NPROC setrlimit.
      } else if (o.isolatePidsMax.find_first_not_of("0123456789") != std::string::npos) {
        std::cerr << "snowglobe run: --pids-max needs an integer or max\n";
        return kExUsage;
      } else {
        try {
          isoPidsMax = std::stol(o.isolatePidsMax);
        } catch (...) {
          std::cerr << "snowglobe run: --pids-max needs an integer or max\n";
          return kExUsage;
        }
      }
    }
    std::error_code rec;
    const fs::path runAbs = fs::absolute(runDir, rec);
    if (rec) {
      std::cerr << "snowglobe run: cannot resolve run dir: " << rec.message() << "\n";
      return kExSoftware;
    }
    std::string prepErr;
    if (!snowglobe::isolate::prepareRunDir(runAbs.string(), fsRwAbs, isoDirs, isoFsRw, prepErr)) {
      std::cerr << "snowglobe run: " << prepErr << "\n";
      return kExSoftware;
    }
#else
    std::cerr << "snowglobe run: --isolate requires Linux (EX_UNAVAILABLE)\n";
    return kExUnavailable;
#endif
#ifdef __linux__
    if (o.isolate) {
      isoMasks = snowglobe::isolate::defaultSecretMasks(isoHomeAbs, isoAllowAbs);
    }
#endif
  }

  // --upstream PROVIDER=URL overrides (gateway testing). Keys lowercased;
  // values must be http(s) URLs.
  std::map<std::string, std::string> upstreamMap;
  for (const std::string& u : o.upstreams) {
    const size_t eq = u.find('=');
    if (eq == std::string::npos || eq == 0) {
      std::cerr << "snowglobe run: --upstream needs PROVIDER=URL, got '" << u << "'\n";
      return kExUsage;
    }
    std::string provider = u.substr(0, eq);
    for (char& c : provider) {
      c = static_cast<char>(std::tolower((unsigned char)c));
    }
    const std::string url = u.substr(eq + 1);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
      std::cerr << "snowglobe run: --upstream URL must be http(s), got '" << url << "'\n";
      return kExUsage;
    }
    upstreamMap[provider] = url;
  }
  // run.meta cmd is secret-redacted (ADR-0003); built after proxy env
  // injection below (superset of secret names, safer). The initial manifest
  // is written after cmdJson exists (see below).
  std::string cmdJson; // filled once secrets are collected

  const std::string manifestPath = (runDir / "manifest.json").string();
  // Manifest isolate record (ADR-0007): {} exactly as before without the
  // flag, so non-isolate manifests (and goldens) stay byte-identical.
  // With --isolate, adds upper + masks (masked secret paths, minus
  // --allow-path) + allow_path (exemptions) for audit.
  std::string isolateJson = "{}";
  if (o.isolate) {
    std::string masksArr = "[";
    for (size_t i = 0; i < isoMasks.size(); ++i) {
      if (i > 0) {
        masksArr += ",";
      }
      masksArr += jsonEscape(isoMasks[i]);
    }
    masksArr += "]";
    std::string allowArr = "[";
    for (size_t i = 0; i < isoAllowAbs.size(); ++i) {
      if (i > 0) {
        allowArr += ",";
      }
      allowArr += jsonEscape(isoAllowAbs[i]);
    }
    allowArr += "]";
    isolateJson = "{\"on\":true,\"features\":[\"userns\",\"mount\",\"pid\",\"overlay\"],"
                  "\"upper\":\"overlay/upper\",\"masks\":" +
                  masksArr + ",\"allow_path\":" + allowArr + "}";
  }
  auto writeManifest = [&](const std::string& finished, uint64_t eventCount,
                           const std::string& lastHash) {
    std::ofstream m(manifestPath, std::ios::trunc);
    m << "{\"schema\":" << SNOWGLOBE_SCHEMA_VERSION
      << ",\"snowglobe_version\":" << jsonEscape(SNOWGLOBE_VERSION)
      << ",\"started\":" << jsonEscape(started) << ",\"finished\":" << finished << ",\"cmd\":["
      << cmdJson << "],\"cwd\":" << jsonEscape(cwd) << ",\"project\":" << jsonEscape(project)
      << ",\"kernel\":" << jsonEscape(kernelStr()) << ",\"tracer\":" << jsonEscape(tracerName)
      << ",\"isolate\":" << isolateJson << ",\"env_fingerprint\":" << jsonEscape(envFingerprint())
      << ",\"event_count\":" << eventCount << ",\"last_hash\":" << jsonEscape(lastHash)
      << ",\"file_hashes\":{}}";
  };

  snowglobe::trace::JsonlWriter writer((runDir / "events.jsonl").string());
  if (!writer.ok()) {
    std::cerr << "snowglobe run: " << writer.error() << "\n";
    return kExSoftware;
  }

  std::unique_ptr<snowglobe::tracer::ITracer> tracer(snowglobe::tracer::PtraceTracer::create());
  if (!tracer) {
    std::cerr << "snowglobe run: ptrace backend requires Linux (EX_UNAVAILABLE)\n";
    return kExUnavailable;
  }

  // --isolate middle spawn happens HERE, while still single-threaded
  // (before the proxy pool exists): forking later inherits userspace
  // locks held by proxy threads and deadlocks the middle (proven
  // in-suite under sanitizers). The middle sets up, stops at SIGSTOP,
  // and waits; the tracer adopts it below after the proxy is up.
  // Best-effort cgroup placement also happens here (pre-init-fork).
  // The guard rmdirs the cgroup at every exit below (best-effort).
  struct CgroupGuard {
    std::string path;
    ~CgroupGuard() {
      if (!path.empty()) {
        ::rmdir(path.c_str());
      }
    }
  };
  CgroupGuard cgGuard;
  pid_t isoChild = -1;
  snowglobe::isolate::ChildPipes ipipes; // isolate only; lives to tracer adopt
  if (o.isolate) {
    std::string pipeErr;
    if (!snowglobe::isolate::makePipes(ipipes, pipeErr)) {
      std::cerr << "snowglobe run: " << pipeErr << "\n";
      return kExSoftware;
    }
    snowglobe::isolate::ChildConfig cfg;
    cfg.projectDir = isoProjectAbs;
    cfg.homeDir = isoHomeAbs;
    cfg.dirs = isoDirs;
    for (const auto& m : isoFsRw) {
      snowglobe::isolate::FsRwMount fm;
      fm.path = m.path;
      fm.upper = m.upper;
      fm.work = m.work;
      cfg.fsRw.push_back(fm);
    }
    cfg.allowEnv = o.isolateAllowEnv;
    cfg.allowPath = isoAllowAbs;
    cfg.pidsMax = isoPidsMax;
    cfg.nofileMax = 1024;
    cfg.memBytes = isoMemBytes;
    cfg.memExplicit = isoMemExplicit;
    cfg.cmd = o.cmd;
    cfg.mapReqW = ipipes.mapReqW;
    cfg.mapAckR = ipipes.mapAckR;
    cfg.statusW = ipipes.statusW;
    cfg.envR = ipipes.envR;
    std::string spawnErr;
    isoChild = snowglobe::isolate::spawnMiddle(cfg, ipipes, spawnErr);
    if (isoChild < 0) {
      std::cerr << "snowglobe run: " << spawnErr << "\n";
      return kExSoftware;
    }
    std::string mapErr;
    // Maps now; setup status later: the middle blocks reading injected
    // env (forwarded after the proxy starts) before reporting ready.
    if (!snowglobe::isolate::serveMaps(isoChild, ipipes, mapErr)) {
      std::cerr << snowglobe::doctor::isolateReport();
      std::cerr << "snowglobe run: " << mapErr << "\n";
      int st = 0;
      ::waitpid(isoChild, &st, 0);
      return kExUnavailable;
    }
    {
      // cgroup wants concrete limits (2G/512 defaults); prlimit fallback in
      // the child uses isoMemBytes/isoPidsMax directly (AS only when explicit).
      const long long memForCg = isoMemExplicit ? isoMemBytes : (2LL * 1024 * 1024 * 1024);
      const long long pidsForCg = isoPidsMax;
      std::string cgNote;
      if (snowglobe::isolate::joinCgroup(runDir.filename().string(), (int)isoChild, memForCg,
                                         pidsForCg, cgNote, cgGuard.path)) {
        std::cerr << "note: cgroup limits active (" << cgNote << ")\n";
      } else {
        // joinCgroup already notes delegation denial; append the prlimit
        // fallback state so `doctor` and run logs agree.
        std::cerr << "note: " << cgNote << " (prlimit fallback active: NPROC=" << isoPidsMax
                  << " NOFILE=1024" << (isoMemExplicit ? " AS=explicit" : " AS=unlimited") << ")\n";
      }
    }
  }

  // LLM proxy: it must be listening before the agent's first request,
  // and the env injection below is inherited by non-isolate children
  // across fork (the isolate middle forked earlier and gets injected
  // env forwarded over a pipe instead). A proxy that cannot bind
  // is a hard error — silently running without capture would fake the
  // recording contract.
  std::unique_ptr<snowglobe::proxy::LlmProxy> proxy;
  long proxyPort = -1;
  if (!o.noLlmProxy) {
    std::error_code llmEc;
    fs::create_directories(runDir / "llm", llmEc);
    if (llmEc) {
      std::cerr << "snowglobe run: cannot create llm dir: " << llmEc.message() << "\n";
      return kExSoftware;
    }
  }

  uint64_t seq = 0;
  bool writeWarned = false;
  std::mutex sinkMu; // proxy handler threads + tracer thread share this sink
  snowglobe::tracer::ITracer::EventSink sink = [&](const std::string& json) {
    std::lock_guard<std::mutex> lk(sinkMu);
    if (writer.writeEvent(seq++, json)) {
      return true;
    }
    // The trace stays prefix-consistent (counts match what was written),
    // but say so loudly instead of losing events silently.
    if (!writeWarned) {
      writeWarned = true;
      std::cerr << "snowglobe run: trace write failed (" << writer.error() << "); continuing\n";
    }
    return false;
  };
  tracer->setSink(sink);

  if (!o.noLlmProxy) {
    snowglobe::proxy::ProxyOptions popts;
    popts.runDir = runDir.string();
    popts.upstream = upstreamMap;
    popts.sink = sink;
    proxy = std::make_unique<snowglobe::proxy::LlmProxy>(std::move(popts));
    proxyPort = proxy->start();
    if (proxyPort <= 0) {
      std::cerr << "snowglobe run: proxy failed: " << proxy->error() << "\n";
      return kExSoftware;
    }
    // Base-URL injection for LLM capture (overwrites any user setting:
    // capture requires our proxy; --no-llm-proxy opts out entirely).
    // OpenAI-conformant SDKs append "/chat/completions" (unversioned) to the
    // base, so the injected base carries the "/v1" — without it every real
    // OpenAI call would misroute to api.openai.com/chat/completions (404).
    // Anthropic/Gemini SDKs version their own paths, so those bases stay bare.
    const std::string base = "http://127.0.0.1:" + std::to_string(proxyPort);
    const std::vector<std::pair<std::string, std::string>> injected = {
        {"OPENAI_BASE_URL", base + "/openai/v1"},     {"OPENAI_API_BASE", base + "/openai/v1"},
        {"ANTHROPIC_BASE_URL", base + "/anthropic"},  {"ANTHROPIC_API_BASE", base + "/anthropic"},
        {"GOOGLE_GEMINI_BASE_URL", base + "/gemini"}, {"GEMINI_API_BASE", base + "/gemini"},
    };
    for (const auto& kv : injected) {
      ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
    }
    if (o.isolate) {
      // The middle forked before these existed: forward them over the env
      // pipe, then read setup status (the middle blocks on env first).
      // An EPIPE here means the middle already died in setup: fall through
      // to awaitReady so its ERR (not our EPIPE) names the real cause.
      std::string envErr;
      if (!snowglobe::isolate::writeEnvBlock(ipipes.envW, injected, envErr)) {
        std::string readyErr;
        if (!snowglobe::isolate::awaitReady(isoChild, ipipes, readyErr)) {
          std::cerr << snowglobe::doctor::isolateReport();
          std::cerr << "snowglobe run: " << readyErr << "\n";
          int st = 0;
          ::waitpid(isoChild, &st, 0);
          return kExUnavailable;
        }
        std::cerr << "snowglobe run: " << envErr << "\n";
        return kExSoftware;
      }
    }
  }
  if (o.isolate && o.noLlmProxy) {
    // No proxy, no injection — still close the middle's env wait with an
    // empty block (a missing write would hang the run, never skip this).
    std::string envErr;
    if (!snowglobe::isolate::writeEnvBlock(
            ipipes.envW, std::vector<std::pair<std::string, std::string>>(), envErr)) {
      std::string readyErr;
      if (!snowglobe::isolate::awaitReady(isoChild, ipipes, readyErr)) {
        std::cerr << snowglobe::doctor::isolateReport();
        std::cerr << "snowglobe run: " << readyErr << "\n";
        int st = 0;
        ::waitpid(isoChild, &st, 0);
        return kExUnavailable;
      }
      std::cerr << "snowglobe run: " << envErr << "\n";
      return kExSoftware;
    }
  }
  if (o.isolate) {
    std::string readyErr;
    if (!snowglobe::isolate::awaitReady(isoChild, ipipes, readyErr)) {
      std::cerr << snowglobe::doctor::isolateReport();
      std::cerr << "snowglobe run: " << readyErr << "\n";
      int st = 0;
      ::waitpid(isoChild, &st, 0);
      return kExUnavailable;
    }
  }

  // Secret env collected AFTER injection (superset, safer); redacts run.meta
  // cmd here and proc.exec argv in the tracer.
  std::vector<std::pair<std::string, std::string>> secrets;
#ifndef _WIN32
  secrets = snowglobe::redact::sensitiveEnv(::environ);
#endif
  for (const auto& a : o.cmd) {
    if (!cmdJson.empty()) {
      cmdJson += ",";
    }
    cmdJson += jsonEscape(snowglobe::redact::redactText(a, secrets));
  }
  writeManifest("null", 0, "0");

  snowglobe::tracer::TraceOptions topts;
  topts.allOpens = o.allOpens;
  topts.tracer = tracerName;
  topts.secretEnv = secrets;
  if (o.isolate) {
    // The middle was spawned (and handshaked) above, pre-threads; the
    // tracer adopts its pid and starts the wait loop at its SIGSTOP.
    topts.isolate = true;
    topts.isolateChild = isoChild;
    topts.isolateMasks = isoMasks;
  }

  if (o.captureStdio) {
    // Phase 0: stdio passthrough; schema files created empty (see issue #7).
    std::ofstream((runDir / "stdout.log")).close();
    std::ofstream((runDir / "stderr.log")).close();
    std::cerr << "note: --capture-stdio passthrough in Phase 0 (logs empty, see issue #7)\n";
  }

  const int code = tracer->run(o.cmd, topts);
  // Drain in-flight LLM streams (<= 10 s) before finalising: their events
  // belong in this trace's counts.
  long turns = 0, errors = 0;
  if (proxy) {
    proxy->stop(10);
    turns = proxy->turns();
    errors = proxy->errors();
  }
  if (code < 0) {
    if (code == -kExUnavailable) {
      if (o.isolate) {
        // Echo the same readiness list doctor --isolate prints, then the
        // specific step + errno, so the missing capability is named twice.
        std::cerr << snowglobe::doctor::isolateReport();
      }
      std::cerr << "snowglobe run: " << tracer->error() << "\n";
      return kExUnavailable;
    }
    std::cerr << "snowglobe run: tracer failed: " << tracer->error() << "\n";
    return kExSoftware;
  }

  const std::string finished = runTimestamp();
  writeManifest(jsonEscape(finished), writer.count(), writer.lastHash());

  if (o.json) {
    std::cout << "{\"run\":" << jsonEscape(runDir.string()) << ",\"exit_code\":" << code
              << ",\"events\":" << writer.count() << ",\"turns\":" << turns
              << ",\"errors\":" << errors << "}\n";
  } else {
    std::cerr << "run: " << runDir.string() << "\n";
    std::cerr << "exit: " << code << " | events: " << writer.count() << " | " << turns
              << " LLM turn" << (turns == 1 ? "" : "s");
    if (errors > 0) {
      std::cerr << " (" << errors << " error" << (errors == 1 ? "" : "s") << ")";
    }
    std::cerr << "\n";
    std::cerr << "next: snowglobe diff " << runDir.string() << " | snowglobe view "
              << runDir.string() << " | snowglobe replay " << runDir.string() << "\n";
  }
  return code; // propagate agent exit code (AGENTS.md §2)
}

int cmdLs(bool json) {
  const fs::path root = runsRoot();
  std::vector<std::string> runs;
  std::error_code ec;
  if (fs::exists(root, ec)) {
    for (const auto& e : fs::directory_iterator(root, ec)) {
      if (e.is_directory()) {
        runs.push_back(e.path().string());
      }
    }
  }
  std::sort(runs.begin(), runs.end());
  if (json) {
    std::cout << "[";
    for (std::size_t i = 0; i < runs.size(); ++i) {
      if (i > 0) {
        std::cout << ",";
      }
      std::cout << jsonEscape(runs[i]);
    }
    std::cout << "]\n";
  } else {
    for (const auto& r : runs) {
      std::cerr << r << "\n";
    }
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef __linux__
  // A supervisor must never die of SIGPIPE: pipe writes to a dead middle
  // (or closed stdout) surface as EPIPE on the next write and flow into
  // the normal 69/70 error paths instead of an anonymous -13.
  ::signal(SIGPIPE, SIG_IGN);
#endif
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty() || args[0] == "-h" || args[0] == "--help") {
    usage(std::cerr);
    return args.empty() ? kExUsage : 0;
  }
  const std::string sub = args[0];

  if (sub == "doctor") {
    if (args.size() > 1 && args[1] == "--isolate") {
      return snowglobe::doctor::printIsolateTable();
    }
    return snowglobe::doctor::printTable();
  }
  if (sub == "version") {
    const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
    if (json) {
      std::cout << "{\"version\":" << jsonEscape(SNOWGLOBE_VERSION)
                << ",\"schema\":" << SNOWGLOBE_SCHEMA_VERSION << "}\n";
    } else {
      std::cerr << "snowglobe " << SNOWGLOBE_VERSION << "\n";
    }
    return 0;
  }
  if (sub == "ls") {
    const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
    return cmdLs(json);
  }
  if (sub == "rm") {
    if (args.size() < 2) {
      std::cerr << "usage: snowglobe rm <run>\n";
      return kExUsage;
    }
    std::error_code ec;
    fs::remove_all(fs::path(args[1]), ec);
    if (ec) {
      std::cerr << "snowglobe rm: " << ec.message() << "\n";
      return kExSoftware;
    }
    return 0;
  }
  if (sub == "run") {
    RunOptions o;
    std::size_t i = 1;
    for (; i < args.size(); ++i) {
      const std::string& a = args[i];
      if (a == "--") {
        ++i;
        break;
      }
      if (a == "-a" || a == "--all-opens") {
        o.allOpens = true;
      } else if (a == "--capture-stdio") {
        o.captureStdio = true;
      } else if (a == "--isolate") {
        o.isolate = true;
      } else if (a.rfind("--fs-rw=", 0) == 0) {
        o.isolateFsRw.push_back(a.substr(8));
      } else if (a == "--fs-rw") {
        if (i + 1 >= args.size()) {
          std::cerr << "snowglobe run: --fs-rw needs PATH\n";
          return kExUsage;
        }
        o.isolateFsRw.push_back(args[++i]);
      } else if (a.rfind("--allow-env=", 0) == 0) {
        o.isolateAllowEnv.push_back(a.substr(12));
      } else if (a == "--allow-env") {
        if (i + 1 >= args.size()) {
          std::cerr << "snowglobe run: --allow-env needs NAME\n";
          return kExUsage;
        }
        o.isolateAllowEnv.push_back(args[++i]);
      } else if (a.rfind("--allow-path=", 0) == 0) {
        o.isolateAllowPath.push_back(a.substr(13));
      } else if (a == "--allow-path") {
        if (i + 1 >= args.size()) {
          std::cerr << "snowglobe run: --allow-path needs PATH\n";
          return kExUsage;
        }
        o.isolateAllowPath.push_back(args[++i]);
      } else if (a.rfind("--memory-max=", 0) == 0) {
        o.isolateMemMax = a.substr(13);
      } else if (a == "--memory-max") {
        if (i + 1 >= args.size()) {
          std::cerr << "snowglobe run: --memory-max needs SIZE\n";
          return kExUsage;
        }
        o.isolateMemMax = args[++i];
      } else if (a.rfind("--pids-max=", 0) == 0) {
        o.isolatePidsMax = a.substr(11);
      } else if (a == "--pids-max") {
        if (i + 1 >= args.size()) {
          std::cerr << "snowglobe run: --pids-max needs N\n";
          return kExUsage;
        }
        o.isolatePidsMax = args[++i];
      } else if (a == "--json") {
        o.json = true;
      } else if (a == "--no-llm-proxy") {
        o.noLlmProxy = true;
      } else if (a.rfind("--upstream=", 0) == 0) {
        o.upstreams.push_back(a.substr(11));
      } else if (a == "--upstream") {
        if (i + 1 >= args.size()) {
          std::cerr << "snowglobe run: --upstream needs PROVIDER=URL\n";
          return kExUsage;
        }
        o.upstreams.push_back(args[++i]);
      } else if (a.rfind("--project=", 0) == 0) {
        o.project = a.substr(10);
      } else if (a.rfind("--out=", 0) == 0) {
        o.out = a.substr(6);
      } else if (a.rfind("--tracer=", 0) == 0) {
        o.tracer = a.substr(9);
      } else if (a == "--project" || a == "--out" || a == "--tracer") {
        if (i + 1 >= args.size()) {
          std::cerr << "snowglobe run: " << a << " needs a value\n";
          return kExUsage;
        }
        const std::string v = args[++i];
        if (a == "--project") {
          o.project = v;
        } else if (a == "--out") {
          o.out = v;
        } else {
          o.tracer = v;
        }
      } else if (a.rfind("--", 0) == 0) {
        std::cerr << "note: ignoring unsupported Phase-0 flag " << a << "\n";
      } else {
        std::cerr << "snowglobe run: unexpected argument '" << a
                  << "' (did you mean -- <command>?)\n";
        return kExUsage;
      }
    }
    for (; i < args.size(); ++i) {
      o.cmd.push_back(args[i]);
    }
    return cmdRun(o);
  }
  if (sub == "view") {
    return snowglobe::view::cmdView(args);
  }
  if (sub == "link") {
    return snowglobe::link::cmdLink(args);
  }
  if (sub == "diff" || sub == "apply" || sub == "replay" || sub == "compare" || sub == "share") {
    std::cerr << "snowglobe " << sub << ": not yet implemented (Phase 2+)\n";
    return kExUnavailable;
  }
  std::cerr << "unknown subcommand '" << sub << "'\n";
  usage(std::cerr);
  return kExUsage;
}

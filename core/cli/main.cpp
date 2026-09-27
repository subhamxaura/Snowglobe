// snowglobe CLI — run (tracer + LLM proxy) + doctor + version + ls/rm stubs.
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
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include "../doctor/doctor.hpp"
#include "../proxy/proxy.hpp"
#include "../redact/redact.hpp"
#include "../trace/jsonl_writer.hpp"
#include "../tracer/common/itracer.hpp"
#include "../tracer/ptrace/ptrace_tracer.hpp"
#include "../util/sha256.hpp"
#include "../util/string_util.hpp"
#include "../version.hpp"

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
     << "                [--capture-stdio] [--json] [--no-llm-proxy]\n"
     << "                [--upstream=PROVIDER=URL]... -- <command> [args...]\n"
     << "  snowglobe doctor\n"
     << "  snowglobe version [--json]\n"
     << "  snowglobe ls [--json]\n"
     << "  snowglobe rm <run>\n"
     << "  snowglobe view|diff|apply|replay|compare|share <run> ... (not yet implemented)\n";
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
  std::vector<std::string> upstreams; // raw PROVIDER=URL strings
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
  auto writeManifest = [&](const std::string& finished, uint64_t eventCount,
                           const std::string& lastHash) {
    std::ofstream m(manifestPath, std::ios::trunc);
    m << "{\"schema\":" << SNOWGLOBE_SCHEMA_VERSION
      << ",\"snowglobe_version\":" << jsonEscape(SNOWGLOBE_VERSION)
      << ",\"started\":" << jsonEscape(started) << ",\"finished\":" << finished << ",\"cmd\":["
      << cmdJson << "],\"cwd\":" << jsonEscape(cwd) << ",\"project\":" << jsonEscape(project)
      << ",\"kernel\":" << jsonEscape(kernelStr()) << ",\"tracer\":" << jsonEscape(tracerName)
      << ",\"isolate\":{},\"env_fingerprint\":" << jsonEscape(envFingerprint())
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

  // LLM proxy first: it must be listening before the child starts, and the
  // env injection below is inherited across fork. A proxy that cannot bind
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
    const std::string base = "http://127.0.0.1:" + std::to_string(proxyPort);
    ::setenv("OPENAI_BASE_URL", (base + "/openai").c_str(), 1);
    ::setenv("OPENAI_API_BASE", (base + "/openai").c_str(), 1);
    ::setenv("ANTHROPIC_BASE_URL", (base + "/anthropic").c_str(), 1);
    ::setenv("ANTHROPIC_API_BASE", (base + "/anthropic").c_str(), 1);
    ::setenv("GOOGLE_GEMINI_BASE_URL", (base + "/gemini").c_str(), 1);
    ::setenv("GEMINI_API_BASE", (base + "/gemini").c_str(), 1);
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

  if (o.captureStdio) {
    // Phase 0: stdio passthrough; schema files created empty (see issue #7).
    std::ofstream((runDir / "stdout.log")).close();
    std::ofstream((runDir / "stderr.log")).close();
    std::cerr << "note: --capture-stdio passthrough in Phase 0 (logs empty, see issue #7)\n";
  }

  const int code = tracer->run(o.cmd, topts);
  // Drain in-flight LLM streams (<= 10 s) before finalising: their events
  // belong in this trace's counts.
  long turns = 0;
  if (proxy) {
    proxy->stop(10);
    turns = proxy->turns();
  }
  if (code < 0) {
    if (code == -kExUnavailable) {
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
              << ",\"events\":" << writer.count() << ",\"turns\":" << turns << "}\n";
  } else {
    std::cerr << "run: " << runDir.string() << "\n";
    std::cerr << "exit: " << code << " | events: " << writer.count() << " | " << turns
              << " LLM turns\n";
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
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty() || args[0] == "-h" || args[0] == "--help") {
    usage(std::cerr);
    return args.empty() ? kExUsage : 0;
  }
  const std::string sub = args[0];

  if (sub == "doctor") {
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
  if (sub == "view" || sub == "diff" || sub == "apply" || sub == "replay" || sub == "compare" ||
      sub == "share") {
    std::cerr << "snowglobe " << sub << ": not yet implemented (Phase "
              << (sub == "view" ? "1C" : "2+") << ")\n";
    return kExUnavailable;
  }
  std::cerr << "unknown subcommand '" << sub << "'\n";
  usage(std::cerr);
  return kExUsage;
}

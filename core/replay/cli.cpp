// snowglobe replay-proxy (standalone stub server) + replay helpers.
#include "cli.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "../proxy/proxy.hpp"
#include "../util/string_util.hpp"
#include "replay_compare.hpp"
#include "replay_match.hpp"

namespace snowglobe::replay {
namespace {

namespace fs = std::filesystem;

constexpr int kExUsage = 64;
constexpr int kExDiverged = 65; // replay verdict: diverged or unrecorded
constexpr int kExUnavailable = 69;
constexpr int kExSoftware = 70;

std::atomic<bool> g_stop{false};
void onSignal(int) {
  g_stop.store(true);
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

bool isRunDir(const std::string& p) {
  std::error_code ec;
  return fs::is_directory(fs::path(p), ec) && !ec;
}

} // namespace

bool loadOrigCmd(const std::string& origDir, std::vector<std::string>& cmd, std::string& err) {
  cmd.clear();
  std::string eventsText;
  if (!readFile(origDir + "/events.jsonl", eventsText)) {
    err = "cannot read " + origDir + "/events.jsonl";
    return false;
  }
  std::istringstream lines(eventsText);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.find("\"run.meta\"") == std::string::npos) {
      continue;
    }
    JsonValue v;
    std::string perr;
    if (!parseJson(line, v, perr) || v.type != JsonValue::Type::Object) {
      continue; // not the meta line in a parseable form; keep scanning
    }
    const auto ev = v.fields.find("ev");
    if (ev == v.fields.end() || ev->second.type != JsonValue::Type::String ||
        ev->second.str != "run.meta") {
      continue;
    }
    const auto c = v.fields.find("cmd");
    if (c == v.fields.end() || c->second.type != JsonValue::Type::Array ||
        c->second.items.empty()) {
      err = "run.meta has no cmd (pass a -- <command> override)";
      return false;
    }
    for (const auto& item : c->second.items) {
      if (item.type != JsonValue::Type::String) {
        err = "run.meta cmd is not a string array (pass a -- <command> override)";
        return false;
      }
      cmd.push_back(item.str);
    }
    return true;
  }
  err = "no run.meta in " + origDir + "/events.jsonl (pass a -- <command> override)";
  return false;
}

bool checkReplayable(const std::string& origDir, std::string& err) {
  std::string eventsText;
  if (!readFile(origDir + "/events.jsonl", eventsText)) {
    err = "replay impossible: cannot read " + origDir + "/events.jsonl";
    return false;
  }
  if (eventsText.find("\"llm.request\"") == std::string::npos) {
    err = "replay impossible: " + origDir + " has no llm.request events (no LLM layer recorded)";
    return false;
  }
  bool haveReq = false;
  std::error_code ec;
  const fs::path llmDir = fs::path(origDir) / "llm";
  if (fs::is_directory(llmDir, ec) && !ec) {
    for (const auto& e : fs::directory_iterator(llmDir, ec)) {
      const std::string name = e.path().filename().string();
      if (name.size() > 9 && name.compare(name.size() - 9, 9, ".req.json") == 0) {
        haveReq = true;
        break;
      }
    }
  }
  if (!haveReq) {
    err = "replay impossible: " + origDir + "/llm has no request envelopes";
    return false;
  }
  return true;
}

int cmdReplayProxy(const std::vector<std::string>& args) {
  if (args.size() < 2 || args.size() > 3) {
    std::cerr << "usage: snowglobe replay-proxy <run> [--realtime]\n";
    return kExUsage;
  }
  bool realtime = false;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--realtime") {
      realtime = true;
    } else {
      std::cerr << "snowglobe replay-proxy: unexpected argument '" << args[i] << "'\n";
      return kExUsage;
    }
  }
  std::string runDir = args[1];
  while (runDir.size() > 1 && runDir.back() == '/') {
    runDir.pop_back();
  }
  if (!isRunDir(runDir)) {
    std::cerr << "snowglobe replay-proxy: no such run '" << args[1] << "'\n";
    return kExUsage;
  }
  std::string why;
  if (!checkReplayable(runDir, why)) {
    std::cerr << "snowglobe replay-proxy: " << why << "\n";
    return kExUnavailable;
  }
  // Scratch dir for this server's own request/response copies (the
  // original run is never written except the unrecorded log). Discarded
  // at shutdown; a crash leaves /tmp debris, never trace corruption.
  char tmpl[] = "/tmp/sg-replay-proxy-XXXXXX";
  if (::mkdtemp(tmpl) == nullptr) {
    std::cerr << "snowglobe replay-proxy: cannot create scratch dir\n";
    return kExSoftware;
  }
  const std::string scratch = tmpl;
  std::error_code ec;
  fs::create_directories(fs::path(scratch) / "llm", ec);
  if (ec) {
    std::cerr << "snowglobe replay-proxy: cannot create scratch llm dir\n";
    return kExSoftware;
  }
  proxy::ProxyOptions popts;
  popts.runDir = scratch;
  popts.replay = true;
  popts.replayRunDir = runDir;
  popts.replayRealtime = realtime;
  proxy::LlmProxy proxy(std::move(popts));
  const long port = proxy.start();
  if (port <= 0) {
    std::cerr << "snowglobe replay-proxy: " << proxy.error() << "\n";
    fs::remove_all(scratch, ec);
    return kExSoftware;
  }
  std::cerr << "replay-proxy: serving " << runDir << " (" << proxy.replayRecorded()
            << " recorded) on 127.0.0.1:" << port << " (offline, no upstream)\n";
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  proxy.stop(10);
  std::cerr << "offline: " << proxy.replayServed() << " responses served, "
            << proxy.replayUnrecorded() << " unrecorded\n";
  fs::remove_all(scratch, ec);
  return 0;
}

int finishReplay(const std::string& origDir, const std::string& newDir,
                 const std::vector<std::string>& ignores, bool asJson) {
  CompareOptions copts;
  copts.ignoreFields = ignores;
  ReplayReport rep;
  std::string err;
  if (!compareReplay(origDir, newDir, copts, rep, err)) {
    std::cerr << "snowglobe replay: compare failed: " << err << "\n";
    return kExSoftware;
  }
  {
    std::ofstream f(newDir + "/replay-report.json", std::ios::trunc | std::ios::binary);
    if (!f) {
      std::cerr << "snowglobe replay: cannot write " << newDir << "/replay-report.json\n";
      return kExSoftware;
    }
    const std::string body = rep.toJson();
    f << body;
    f.flush();
    if (!f) {
      std::cerr << "snowglobe replay: cannot write " << newDir << "/replay-report.json\n";
      return kExSoftware;
    }
  }
  if (asJson) {
    std::cout << rep.toJson() << "\n";
  }
  auto show = [](const char* name, const CategoryResult& c) {
    std::cerr << "  " << name << ": " << c.status;
    if (!c.detail.empty()) {
      std::cerr << " (" << c.detail << ")";
    }
    std::cerr << "\n";
    for (const auto& e : c.replayOnly) {
      std::cerr << "    " << e << "\n";
    }
    for (const auto& e : c.originalOnly) {
      std::cerr << "    " << e << "\n";
    }
  };
  std::cerr << "replay: " << newDir << " (replay of " << origDir << ")\n";
  std::cerr << "turns: " << rep.newTurns << "/" << rep.origTurns
            << (rep.newTurns == rep.origTurns ? " match" : " MISMATCH") << " | order "
            << (rep.orderMatches ? "matches" : "DIVERGED") << " | unrecorded: " << rep.unrecorded
            << "\n";
  show("llm", rep.llm);
  show("fs", rep.fs);
  show("proc", rep.proc);
  show("net", rep.net);
  show("exit", rep.exitCat);
  if (rep.originalExit != 0 && rep.originalExit == rep.replayExit) {
    std::cerr << "same failure as original (exit " << rep.replayExit << ")\n";
  }
  if (rep.clean()) {
    std::cerr << "verdict: clean\n";
    std::cerr << "next: snowglobe view " << newDir << " | snowglobe diff " << newDir << "\n";
    return 0;
  }
  std::cerr << "verdict: DIVERGED\n";
  return kExDiverged;
}

} // namespace snowglobe::replay

// snowglobe link implementation (algorithm in linker.cpp; CLI contract
// amendment in ADR-0006). Single-threaded: reads events, then blobs.
#include "cli.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "../util/string_util.hpp"
#include "link.hpp"

namespace snowglobe::link {
namespace {

namespace fs = std::filesystem;

constexpr int kExUsage = 64;
constexpr int kExSoftware = 70;
constexpr int kExStale = 3; // --check: links.json missing or outdated

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

// Blob paths come from trace events: confine to runDir (same rule as
// view's resolveBlob — no "..", no absolute, must be a regular file).
bool confinedBlob(const std::string& runDir, const std::string& rel, std::string& absOut) {
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
  const fs::path cand = fs::path(runDir) / rel;
  if (!fs::is_regular_file(cand, ec) || ec) {
    return false;
  }
  absOut = cand.string();
  return true;
}

void printSummary(const std::string& where, const LinksDoc& doc) {
  std::cerr << "links: " << where << "\n";
  std::cerr << "turns: " << doc.turns << " | attributed: " << doc.attributed << " (";
  bool first = true;
  for (const auto& kv : doc.basisCounts) {
    if (!first) {
      std::cerr << ", ";
    }
    first = false;
    std::cerr << kv.first << " " << kv.second;
  }
  std::cerr << ") | unattributed: " << doc.unattributed << " (";
  first = true;
  for (const auto& kv : doc.reasonCounts) {
    if (!first) {
      std::cerr << ", ";
    }
    first = false;
    std::cerr << kv.first << " " << kv.second;
  }
  std::cerr << ") | probes excluded: " << doc.probesExcluded << "\n";
}

} // namespace

int cmdLink(const std::vector<std::string>& args) {
  std::string run;
  bool check = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--check") {
      check = true;
    } else if (args[i].rfind("--", 0) == 0) {
      std::cerr << "snowglobe link: unknown flag '" << args[i] << "'\n";
      return kExUsage;
    } else if (run.empty()) {
      run = args[i];
    } else {
      std::cerr << "usage: snowglobe link <run> [--check]\n";
      return kExUsage;
    }
  }
  if (run.empty()) {
    std::cerr << "usage: snowglobe link <run> [--check]\n";
    return kExUsage;
  }

  // Run source mirrors view: a .sgr directory, or a bare events.jsonl.
  std::error_code ec;
  std::string eventsPath;
  std::string linksPath;
  std::string runDir; // blob root (empty in bare-file mode without llm/)
  if (fs::is_directory(run, ec) && !ec) {
    eventsPath = (fs::path(run) / "events.jsonl").string();
    linksPath = (fs::path(run) / "links.json").string();
    runDir = run;
  } else if (fs::is_regular_file(run, ec) && !ec) {
    eventsPath = run;
    linksPath = (fs::path(run).parent_path() / "links.json").string();
    const fs::path llm = fs::path(run).parent_path() / "llm";
    if (fs::is_directory(llm, ec) && !ec) {
      runDir = fs::path(run).parent_path().string();
    }
  } else {
    std::cerr << "snowglobe link: no such run '" << run << "'\n";
    return kExUsage;
  }

  std::vector<std::string> lines;
  {
    std::ifstream f(eventsPath, std::ios::binary);
    if (!f) {
      std::cerr << "snowglobe link: cannot read " << eventsPath << "\n";
      return kExUsage;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back(); // tolerate CRLF checkouts
      }
      lines.push_back(line);
    }
    if (f.bad()) {
      std::cerr << "snowglobe link: error reading " << eventsPath << "\n";
      return kExSoftware;
    }
  }

  BlobReader blobs = [&](const std::string& rel, std::string& out) {
    std::string abs;
    if (!confinedBlob(runDir, rel, abs)) {
      return false;
    }
    return readFile(abs, out);
  };
  const LinksDoc doc = buildLinks(lines, blobs);

  if (check) {
    std::string current;
    if (readFile(linksPath, current) && current == doc.json) {
      std::cerr << "links: CURRENT " << linksPath << "\n";
      printSummary(linksPath, doc);
      return 0;
    }
    std::cerr << "links: STALE " << linksPath << " (regenerate with snowglobe link " << run
              << ")\n";
    printSummary(linksPath, doc);
    return kExStale;
  }

  {
    std::ofstream f(linksPath, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cerr << "snowglobe link: cannot write " << linksPath << "\n";
      return kExSoftware;
    }
    f << doc.json;
    f.flush();
    if (!f) {
      std::cerr << "snowglobe link: error writing " << linksPath << "\n";
      return kExSoftware;
    }
  }
  printSummary(linksPath, doc);
  return 0;
}

} // namespace snowglobe::link

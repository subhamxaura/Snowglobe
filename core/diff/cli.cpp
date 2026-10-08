// diff / apply / compare command dispatch (ADR-0008).
#include "cli.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../link/event_scan.hpp"
#include "../util/string_util.hpp"
#include "apply.hpp"
#include "baseline.hpp"
#include "changeset.hpp"

namespace snowglobe::diff {
namespace {

constexpr int kExUsage = 64;
constexpr int kExConflict = 65; // apply aborted: conflicts/rejections, zero writes
constexpr int kExUnavailable = 69;
constexpr int kExSoftware = 70;

bool isDir(const std::string& p) {
  struct stat st = {};
  return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& p) {
  struct stat st = {};
  return ::lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Resolve the diff target: a .sgr dir, or a bare events.jsonl file
// (event-mode, like `view`).
bool resolveRun(const std::string& arg, std::string& runDir, std::string& eventsFile,
                bool& bareFile) {
  bareFile = false;
  if (isFile(arg)) {
    runDir = "";
    eventsFile = arg;
    bareFile = true;
    return true;
  }
  if (!isDir(arg)) {
    return false;
  }
  runDir = arg;
  while (runDir.size() > 1 && runDir.back() == '/') {
    runDir.pop_back();
  }
  eventsFile = runDir + "/events.jsonl";
  return true;
}

} // namespace

int cmdDiff(const std::vector<std::string>& args) {
  if (args.size() < 2 || args.size() > 4) {
    std::cerr << "usage: snowglobe diff <run> [--stat] [--patch=FILE]\n";
    return kExUsage;
  }
  bool wantPatch = false;
  std::string patchFile;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--stat") {
      continue; // default output already is --stat
    } else if (args[i].rfind("--patch=", 0) == 0) {
      wantPatch = true;
      patchFile = args[i].substr(8);
    } else if (args[i] == "--patch") {
      if (i + 1 >= args.size()) {
        std::cerr << "snowglobe diff: --patch needs FILE\n";
        return kExUsage;
      }
      wantPatch = true;
      patchFile = args[++i];
    } else {
      std::cerr << "snowglobe diff: unexpected argument '" << args[i] << "'\n";
      return kExUsage;
    }
  }
  if (wantPatch && patchFile.empty()) {
    std::cerr << "snowglobe diff: --patch needs FILE\n";
    return kExUsage;
  }
  std::string runDir, eventsFile;
  bool bareFile = false;
  if (!resolveRun(args[1], runDir, eventsFile, bareFile)) {
    std::cerr << "snowglobe diff: no such run '" << args[1] << "'\n";
    return kExUsage;
  }
  const bool hasBaseline =
      !bareFile && isFile(runDir + "/baseline.json") && isDir(runDir + "/overlay");
  if (!hasBaseline) {
    // Honest mode (ADR-0008): event-derived paths, never content.
    if (!isFile(eventsFile)) {
      std::cerr << "snowglobe diff: run has no baseline/upper (and no events.jsonl): " << args[1]
                << "\n";
      return kExUnavailable;
    }
    std::string report, error;
    if (!eventPathReport(eventsFile, report, error)) {
      std::cerr << "snowglobe diff: " << error << "\n";
      return kExSoftware;
    }
    std::cerr << report;
    return 0;
  }
  Baseline baseline;
  std::string error;
  if (!loadBaseline(runDir + "/baseline.json", baseline, error)) {
    std::cerr << "snowglobe diff: " << error << "\n";
    // Missing file = the 69 "no baseline" case; corrupt = 70.
    return (error.find("open ") != std::string::npos) ? kExUnavailable : kExSoftware;
  }
  ChangeSet cs;
  if (!computeChanges(runDir, baseline.project, baseline, true, cs, error)) {
    std::cerr << "snowglobe diff: " << error << "\n";
    return kExSoftware;
  }
  std::cerr << renderStat(cs);
  if (wantPatch) {
    std::string patch = "# snowglobe diff " + args[1] +
                        "\n# informational: apply reads the upper, not this patch\n";
    size_t omitted = 0;
    for (const Change& c : cs.changes) {
      if (c.kind == 'S' || c.patch.empty()) {
        if (c.kind == 'S') {
          ++omitted;
        } else if (c.kind == 'D' && c.noOldBytes) {
          patch += "# deleted, old content unavailable: " + scopeLabel(c.scope, c.fsRwIndex) + "/" +
                   c.path + "\n";
        } else if (c.hostChanged) {
          patch +=
              "# host changed since run (hunks withheld): " + scopeLabel(c.scope, c.fsRwIndex) +
              "/" + c.path + "\n";
        }
        continue;
      }
      if (c.kind == 'R') {
        patch += "diff --git a/" + c.path + " b/" + c.newPath + "\nrename from " + c.path +
                 "\nrename to " + c.newPath + "\nsimilarity 100%\n";
        continue;
      }
      const std::string scopePfx =
          (c.scope == Scope::Project) ? "" : scopeLabel(c.scope, c.fsRwIndex) + "/";
      if (c.kind == 'A') {
        patch += "diff --git a/" + scopePfx + c.path + " b/" + scopePfx + c.path + "\nnew file\n";
      } else if (c.kind == 'D') {
        patch += "diff --git a/" + scopePfx + c.path + " b/" + scopePfx + c.path + "\ndeleted\n";
      } else {
        patch += "diff --git a/" + scopePfx + c.path + " b/" + scopePfx + c.path + "\n";
      }
      patch += c.patch;
    }
    if (omitted > 0) {
      std::cerr << "note: " << omitted << " special file(s) omitted from patch\n";
    }
    FILE* f = ::fopen(patchFile.c_str(), "wb");
    if (f == nullptr) {
      std::cerr << "snowglobe diff: cannot write " << patchFile << ": " << strerror(errno) << "\n";
      return kExSoftware;
    }
    const bool ok = patch.empty() || ::fwrite(patch.data(), 1, patch.size(), f) == patch.size();
    if (::fclose(f) != 0 || !ok) {
      std::cerr << "snowglobe diff: cannot write " << patchFile << ": " << strerror(errno) << "\n";
      return kExSoftware;
    }
    std::cerr << "wrote " << patch.size() << " patch bytes to " << patchFile << "\n";
  }
  return 0;
}

int cmdApply(const std::vector<std::string>& args) {
  if (args.size() < 2 || args.size() > 4) {
    std::cerr << "usage: snowglobe apply <run> [--dry-run] [--yes]\n";
    return kExUsage;
  }
  bool dryRun = false;
  bool yes = false;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--dry-run") {
      dryRun = true;
    } else if (args[i] == "--yes") {
      yes = true;
    } else {
      std::cerr << "snowglobe apply: unexpected argument '" << args[i] << "'\n";
      return kExUsage;
    }
  }
  const std::string& runArg = args[1];
  if (!isDir(runArg)) {
    std::cerr << "snowglobe apply: no such run '" << runArg << "'\n";
    return kExUsage;
  }
  std::string runDir = runArg;
  while (runDir.size() > 1 && runDir.back() == '/') {
    runDir.pop_back();
  }
  if (!isFile(runDir + "/baseline.json") || !isDir(runDir + "/overlay")) {
    std::cerr << "snowglobe apply: run has no baseline/upper: " << runArg << "\n";
    return kExUnavailable;
  }
  Baseline baseline;
  std::string error;
  if (!loadBaseline(runDir + "/baseline.json", baseline, error)) {
    std::cerr << "snowglobe apply: " << error << "\n";
    return (error.find("open ") != std::string::npos) ? kExUnavailable : kExSoftware;
  }
  ApplyPlan plan;
  if (!planApply(baseline.project, runDir, plan, error)) {
    std::cerr << "snowglobe apply: " << error << "\n";
    return kExSoftware;
  }
  std::cerr << renderApplyReport(plan, dryRun);
  if (!plan.clean()) {
    return kExConflict;
  }
  if (dryRun) {
    return 0;
  }
  if (!yes) {
    if (::isatty(STDIN_FILENO) == 0) {
      std::cerr << "snowglobe apply: refusing without --yes off-tty (non-interactive)\n";
      return kExUsage;
    }
    std::cerr << "Apply " << plan.ops.size() << " change(s) to " << baseline.project << "? [y/N] ";
    std::string answer;
    if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
      std::cerr << "aborted (nothing written)\n";
      return kExUsage;
    }
  }
  if (!execApply(baseline.project, plan, error)) {
    std::cerr << "snowglobe apply: " << error << "\n";
    return kExSoftware;
  }
  std::cerr << "applied " << plan.ops.size() << " change(s) to " << baseline.project << "\n";
  return 0;
}

// Load one run's raw (host-independent) change set for compare, so two
// runs join on agent behavior even after one of them was applied.
namespace {
bool rawChanges(const std::string& runArg, ChangeSet& cs, std::string& error) {
  if (!isDir(runArg)) {
    error = "no such run '" + runArg + "'";
    return false;
  }
  std::string runDir = runArg;
  while (runDir.size() > 1 && runDir.back() == '/') {
    runDir.pop_back();
  }
  if (!isFile(runDir + "/baseline.json") || !isDir(runDir + "/overlay")) {
    error = "run has no baseline/upper: " + runArg;
    return false;
  }
  Baseline baseline;
  if (!loadBaseline(runDir + "/baseline.json", baseline, error)) {
    return false;
  }
  return computeRawChanges(runDir, baseline, cs, error);
}
} // namespace

int cmdCompare(const std::vector<std::string>& args) {
  bool statOnly = false;
  bool asJson = false;
  std::vector<std::string> runs;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--stat") {
      statOnly = true;
    } else if (args[i] == "--json") {
      asJson = true;
    } else if (args[i].rfind("--", 0) == 0) {
      std::cerr << "snowglobe compare: unexpected argument '" << args[i] << "'\n";
      return kExUsage;
    } else {
      runs.push_back(args[i]);
    }
  }
  if (runs.size() != 2) {
    std::cerr << "usage: snowglobe compare <runA> <runB> [--stat] [--json]\n";
    return kExUsage;
  }
  ChangeSet a, b;
  std::string error;
  if (!rawChanges(runs[0], a, error)) {
    std::cerr << "snowglobe compare: " << error << "\n";
    return kExUnavailable;
  }
  if (!rawChanges(runs[1], b, error)) {
    std::cerr << "snowglobe compare: " << error << "\n";
    return kExUnavailable;
  }
  const std::vector<CompareRow> rows = joinChangeSets(a, b);
  if (asJson) {
    std::cout << renderCompareJson(rows) << "\n";
    return 0;
  }
  if (statOnly) {
    size_t nA = 0, nB = 0, nSame = 0, nDiffer = 0;
    for (const CompareRow& r : rows) {
      if (r.inA && !r.inB) {
        ++nA;
      } else if (r.inB && !r.inA) {
        ++nB;
      } else if (r.same) {
        ++nSame;
      } else {
        ++nDiffer;
      }
    }
    std::cerr << rows.size() << " path(s): " << nA << " A-only, " << nB << " B-only, " << nSame
              << " same, " << nDiffer << " differ\n";
    return 0;
  }
  std::cerr << renderCompare(rows);
  return 0;
}

} // namespace snowglobe::diff

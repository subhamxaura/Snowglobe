// snowglobe share / verify command dispatch (ADR-0011).
#include "cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "bundle.hpp"
#include "verify.hpp"

namespace snowglobe::share {
namespace {

namespace fs = std::filesystem;

constexpr int kExUsage = 64;
constexpr int kExRefused = 65; // share refused / verify FAILED
constexpr int kExUnavailable = 69;
constexpr int kExSoftware = 70;

bool isDir(const std::string& p) {
  std::error_code ec;
  return fs::is_directory(fs::path(p), ec) && !ec;
}

bool isFile(const std::string& p) {
  std::error_code ec;
  return fs::is_regular_file(fs::path(p), ec) && !ec;
}

} // namespace

int cmdShare(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::cerr << "usage: snowglobe share <run> [--out=FILE] [--force] [--with-overlay]\n";
    return kExUsage;
  }
  std::string runArg = args[1];
  std::string out;
  bool force = false;
  bool withOverlay = false;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i].rfind("--out=", 0) == 0) {
      out = args[i].substr(6);
    } else if (args[i] == "--out") {
      if (i + 1 >= args.size()) {
        std::cerr << "snowglobe share: --out needs FILE\n";
        return kExUsage;
      }
      out = args[++i];
    } else if (args[i] == "--force") {
      force = true;
    } else if (args[i] == "--with-overlay") {
      withOverlay = true;
    } else {
      std::cerr << "snowglobe share: unexpected argument '" << args[i] << "'\n";
      return kExUsage;
    }
  }
  if (out.empty() && (runArg.empty() || runArg.back() == '/')) {
    while (runArg.size() > 1 && runArg.back() == '/') {
      runArg.pop_back();
    }
  }
  if (runArg.empty()) {
    std::cerr << "snowglobe share: --out needs FILE\n";
    return kExUsage;
  }
  std::error_code ec;
  const fs::path runCanon = fs::canonical(fs::path(runArg), ec);
  if (ec || !isDir(runCanon.string())) {
    std::cerr << "snowglobe share: no such run '" << runArg << "'\n";
    return kExUsage;
  }
  const std::string runDir = runCanon.string();
  if (out.empty()) {
    out = runDir + "x"; // foo.sgr -> foo.sgrx, alongside the run
  }
  std::vector<std::string> notes;
  std::vector<AuditHit> hits;
  std::string err;
  if (!shareRun(runDir, out, force, withOverlay, notes, hits, err)) {
    if (!hits.empty() && !force) {
      // The refusal listing: every finding, capped (the count is exact).
      std::cerr << "snowglobe share: " << err << "\n";
      size_t shown = 0;
      for (const AuditHit& h : hits) {
        if (shown >= 20) {
          std::cerr << "  ... and " << (hits.size() - shown) << " more\n";
          break;
        }
        std::cerr << "  " << h.file << ":" << h.line << " (" << h.kind << ")\n";
        ++shown;
      }
      return kExRefused;
    }
    // collect/verify/pack failures: 69 when inputs are missing, else 65
    // (refused: corrupt chain, clobber refusal) — never silent.
    if (err.find("missing ") != std::string::npos ||
        err.find("nothing to share") != std::string::npos) {
      std::cerr << "snowglobe share: " << err << "\n";
      return kExUnavailable;
    }
    std::cerr << "snowglobe share: " << err << "\n";
    return (err.find("cannot ") != std::string::npos) ? kExSoftware : kExRefused;
  }
  for (const std::string& n : notes) {
    std::cerr << "note: " << n << "\n";
  }
  if (force && !hits.empty()) {
    std::cerr << "warning: sharing with " << hits.size()
              << " secret finding(s) (--force; audit overridden, chain verified)\n";
  }
  std::error_code sec;
  const uint64_t bytes = fs::file_size(out, sec);
  std::cerr << "shared: " << out;
  if (!sec) {
    std::cerr << " (" << bytes << " bytes)";
  }
  std::cerr << "\n";
  std::cerr << "next: snowglobe verify " << out << "\n";
  return 0;
}

int cmdVerify(const std::vector<std::string>& args) {
  if (args.size() < 2 || args.size() > 4) {
    std::cerr << "usage: snowglobe verify <run-or-bundle> [--extract-to DIR]\n";
    return kExUsage;
  }
  std::string target = args[1];
  std::string extractTo;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i].rfind("--extract-to=", 0) == 0) {
      extractTo = args[i].substr(13);
    } else if (args[i] == "--extract-to") {
      if (i + 1 >= args.size()) {
        std::cerr << "snowglobe verify: --extract-to needs DIR\n";
        return kExUsage;
      }
      extractTo = args[++i];
    } else {
      std::cerr << "snowglobe verify: unexpected argument '" << args[i] << "'\n";
      return kExUsage;
    }
  }
  while (target.size() > 1 && target.back() == '/') {
    target.pop_back();
  }
  VerifyResult vr;
  std::string err;
  if (isDir(target)) {
    if (!extractTo.empty()) {
      std::cerr << "snowglobe verify: --extract-to is bundle-only\n";
      return kExUsage;
    }
    if (!verifyRun(target, vr, err)) {
      std::cerr << "snowglobe verify: " << err << "\n";
      return (err.find("cannot read") != std::string::npos) ? kExUnavailable : kExSoftware;
    }
  } else if (isFile(target)) {
    // Bundle: extract (temp, or --extract-to to keep the tree), then
    // verify as a run.
    std::string dest = extractTo;
    bool keep = !extractTo.empty();
    if (!keep) {
      char tmpl[] = "/tmp/sg-verify-XXXXXX";
      if (::mkdtemp(tmpl) == nullptr) {
        std::cerr << "snowglobe verify: cannot create scratch dir\n";
        return kExSoftware;
      }
      dest = tmpl;
    }
    if (!verifyBundle(target, dest, vr, err)) {
      std::cerr << "snowglobe verify: " << err << "\n";
      if (!keep) {
        std::error_code ec;
        fs::remove_all(dest, ec);
      }
      return (err.find("cannot read") != std::string::npos ||
              err.find("cannot create") != std::string::npos)
                 ? kExUnavailable
                 : kExRefused;
    }
    if (!keep) {
      std::error_code ec;
      fs::remove_all(dest, ec);
    } else {
      std::cerr << "extracted: " << dest << "\n";
    }
  } else {
    std::cerr << "snowglobe verify: no such run or bundle '" << args[1] << "'\n";
    return kExUsage;
  }
  if (!vr.ok) {
    std::cerr << "verify: " << target << ": FAILED (" << vr.events << " events)\n";
    for (const std::string& e : vr.errors) {
      std::cerr << "  " << e << "\n";
    }
    return kExRefused;
  }
  std::cerr << "verify: " << target << ": OK (" << vr.events << " events)\n";
  return 0;
}

} // namespace snowglobe::share

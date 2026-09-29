// snowglobe view — localhost server for the embedded viewer + trace files.
// Thread ownership: httplib pool threads run the handler; all shared state
// is read-only after start (embedded table, run dir string). No trace is
// written by view (read-only).
#include "view.hpp"

#ifdef SNOWGLOBE_HAS_VIEWER
#include "snowglobe_viewer_data.hpp"
#endif

#include <httplib.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

#ifdef SNOWGLOBE_HAS_VIEWER
bool startsWith(const std::string& s, const std::string& pre) {
  return s.compare(0, pre.size(), pre) == 0;
}

std::string mimeFor(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  const std::string ext = dot == std::string::npos ? "" : path.substr(dot);
  if (ext == ".html") {
    return "text/html";
  }
  if (ext == ".js" || ext == ".mjs") {
    return "text/javascript";
  }
  if (ext == ".css") {
    return "text/css";
  }
  if (ext == ".json" || ext == ".map") {
    return "application/json";
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
  if (ext == ".txt" || ext == ".idx" || ext == ".log") {
    return "text/plain";
  }
  if (ext == ".sse") {
    return "text/event-stream";
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
    std::cerr << "usage: snowglobe view <run> [--port=7777] [--open]\n";
    return kExUsage;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(run, ec) || !fs::exists(run + "/manifest.json", ec) ||
      !fs::exists(run + "/events.jsonl", ec)) {
    std::cerr << "snowglobe view: not a run dir (need manifest.json + "
                 "events.jsonl): "
              << run << "\n";
    return kExUsage;
  }
#ifndef SNOWGLOBE_HAS_VIEWER
  (void)port;
  (void)open;
  std::cerr << "snowglobe view: this binary was built without the viewer "
               "(SNOWGLOBE_VIEWER=OFF)\n";
  return kExUnavailable;
#else
  httplib::Server svr;
  svr.Get(".*", [&](const httplib::Request& req, httplib::Response& res) {
    std::string p = req.path.empty() ? "/" : req.path;
    if (p == "/") {
      p = "/index.html";
    }
    if (startsWith(p, "/trace/")) {
      // Trace files straight from the run dir. Lexical guard only (no
      // symlink chasing): reject anything that could leave the run dir.
      const std::string rel = p.substr(7);
      if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
        res.status = 404;
        res.set_content("not found", "text/plain");
        return;
      }
      std::string body;
      if (!readFile(run + "/" + rel, body)) {
        res.status = 404;
        res.set_content("not found", "text/plain");
        return;
      }
      res.set_content(body, mimeFor(rel));
      return;
    }
    for (unsigned long i = 0; i < viewer_data::kFileCount; ++i) {
      const auto& f = viewer_data::kFiles[i];
      if (p == f.path) {
        res.set_content(std::string(reinterpret_cast<const char*>(f.data), f.size),
                        mimeFor(f.path));
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
  std::cerr << "view: " << url << " " << run << "\n";
#ifndef _WIN32
  if (open) {
    launchBrowser(url);
  }
#else
  (void)open;
#endif
  svr.listen_after_bind(); // foreground until SIGINT/SIGTERM
  return 0;
#endif
}

} // namespace snowglobe::view

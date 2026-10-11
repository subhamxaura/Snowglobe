// Unit tests: share tar roundtrip/determinism + verify chain/manifest.
// (Audit shapes are pinned in test_redact.cpp; integration pins the CLI.)
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <zstd.h>

#include "share/bundle.hpp"
#include "share/tar.hpp"
#include "share/verify.hpp"
#include "trace/jsonl_writer.hpp"
#include "util/sha256.hpp"

namespace fs = std::filesystem;
using snowglobe::share::TarEntry;

namespace {

std::string tmpDir(const std::string& tag) {
  const fs::path p = fs::temp_directory_path() /
                     ("sg-share-ut-" + tag + "-" + std::to_string(::getpid()) + "-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(p);
  return p.string();
}

void writeFile(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::trunc | std::ios::binary);
  f << data;
  f.flush();
  REQUIRE(f.good());
}

// Minimal valid run: 2 events via the real JsonlWriter + manifest.
struct MiniRun {
  std::string dir;
  std::string lastHash;
  MiniRun() {
    dir = tmpDir("run");
    snowglobe::trace::JsonlWriter w(dir + "/events.jsonl");
    REQUIRE(w.ok());
    REQUIRE(w.writeEvent(0, "{\"ev\":\"proc.exec\",\"path\":\"/bin/true\"}"));
    REQUIRE(w.writeEvent(1, "{\"ev\":\"proc.exit\",\"code\":0}"));
    lastHash = w.lastHash();
    std::ostringstream m;
    m << "{\"schema\":0,\"snowglobe_version\":\"t\",\"started\":\"t\",\"finished\":\"t\",\"cmd\":[]"
         ","
         "\"cwd\":\"/\",\"project\":\"/\",\"kernel\":\"t\",\"tracer\":\"ptrace\",\"isolate\":{},"
         "\"env_fingerprint\":\"x\",\"event_count\":2,\"last_hash\":\""
      << lastHash << "\",\"file_hashes\":{}}";
    writeFile(dir + "/manifest.json", m.str());
  }
  ~MiniRun() {
    fs::remove_all(dir);
  }
};

} // namespace

TEST_CASE("share tar roundtrip", "[share]") {
  using snowglobe::share::tarRead;
  using snowglobe::share::tarWrite;
  std::vector<TarEntry> in;
  TarEntry a;
  a.name = "manifest.json";
  a.data = "{\"a\":1}";
  in.push_back(a);
  TarEntry b;
  b.name = "llm/0000.res.json";
  b.data = std::string("x\0y\0z", 5); // embedded NULs survive
  in.push_back(b);
  TarEntry c;
  c.name = "empty.bin";
  c.data = "";
  in.push_back(c);
  TarEntry d;
  d.name = "llm";
  d.isDir = true;
  d.mode = 0755;
  in.push_back(d);
  TarEntry e;
  e.name = "a/very/deep/nested/path/that/still/fits/in/one/hundred/chars/file.txt";
  e.data = "deep";
  in.push_back(e);
  TarEntry f;
  f.name = "rel/link";
  f.isSymlink = true;
  f.linkTarget = "../manifest.json";
  in.push_back(f);
  // Prefix-split long name (103 chars: tail fits, prefix carries dir).
  TarEntry g;
  g.name = std::string("dd/") + std::string(95, 'n') + ".txt";
  g.data = "long";
  in.push_back(g);
  std::string blob, err;
  REQUIRE(tarWrite(in, blob, err));
  std::vector<TarEntry> out;
  REQUIRE(tarRead(blob, out, err));
  REQUIRE(out.size() == in.size());
  for (size_t k = 0; k < in.size(); ++k) {
    CHECK(out[k].name == in[k].name);
    CHECK(out[k].isDir == in[k].isDir);
    CHECK(out[k].isSymlink == in[k].isSymlink);
    CHECK(out[k].data == in[k].data);
    CHECK(out[k].linkTarget == in[k].linkTarget);
  }
  // Determinism: same entries twice -> identical bytes.
  std::string blob2;
  REQUIRE(tarWrite(in, blob2, err));
  CHECK(blob == blob2);
}

TEST_CASE("share tar rejects loudly", "[share]") {
  using snowglobe::share::tarRead;
  using snowglobe::share::tarWrite;
  std::string out, err;
  TarEntry abs;
  abs.name = "/etc/shadow";
  abs.data = "x";
  CHECK(!tarWrite({abs}, out, err));
  TarEntry dotdot;
  dotdot.name = "a/../../evil";
  dotdot.data = "x";
  CHECK(!tarWrite({dotdot}, out, err));
  TarEntry toolong;
  toolong.name = std::string(300, 'n');
  toolong.data = "x";
  CHECK(!tarWrite({toolong}, out, err));
  // Reader: bad magic and checksum damage fail loud.
  std::vector<TarEntry> entries;
  CHECK(!tarRead(std::string(512, 'A'), entries, err));
  TarEntry good;
  good.name = "f";
  good.data = "hello";
  REQUIRE(tarWrite({good}, out, err));
  out[100] ^= 0x01; // damage the mode field -> checksum mismatch
  CHECK(!tarRead(out, entries, err));
}

TEST_CASE("share verify accepts a writer-built run", "[share]") {
  MiniRun r;
  snowglobe::share::VerifyResult vr;
  std::string err;
  REQUIRE(snowglobe::share::verifyRun(r.dir, vr, err));
  CHECK(vr.ok);
  CHECK(vr.events == 2);
  CHECK(vr.lastHash == r.lastHash);
}

TEST_CASE("share verify rejects tampering", "[share]") {
  // Flip one hex char of the first hash -> chain break at line 1.
  {
    MiniRun r;
    std::string text;
    {
      std::ifstream f(r.dir + "/events.jsonl", std::ios::binary);
      std::ostringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    }
    const size_t h = text.find("\"hash\":\"");
    REQUIRE(h != std::string::npos);
    text[h + 8] = (text[h + 8] == 'a' ? 'b' : 'a');
    writeFile(r.dir + "/events.jsonl", text);
    snowglobe::share::VerifyResult vr;
    std::string err;
    REQUIRE(snowglobe::share::verifyRun(r.dir, vr, err));
    CHECK(!vr.ok);
    CHECK(!vr.errors.empty());
  }
  // Drop a line -> seq gap.
  {
    MiniRun r;
    std::string text;
    {
      std::ifstream f(r.dir + "/events.jsonl", std::ios::binary);
      std::ostringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    }
    text = text.substr(text.find('\n') + 1);
    writeFile(r.dir + "/events.jsonl", text);
    snowglobe::share::VerifyResult vr;
    std::string err;
    REQUIRE(snowglobe::share::verifyRun(r.dir, vr, err));
    CHECK(!vr.ok);
  }
  // Manifest lies about the count -> mismatch.
  {
    MiniRun r;
    std::string m;
    {
      std::ifstream f(r.dir + "/manifest.json", std::ios::binary);
      std::ostringstream ss;
      ss << f.rdbuf();
      m = ss.str();
    }
    const size_t p = m.find("\"event_count\":2");
    REQUIRE(p != std::string::npos);
    m.replace(p, 15, "\"event_count\":3");
    writeFile(r.dir + "/manifest.json", m);
    snowglobe::share::VerifyResult vr;
    std::string err;
    REQUIRE(snowglobe::share::verifyRun(r.dir, vr, err));
    CHECK(!vr.ok);
  }
  // Missing manifest field -> unreadable (69-path, returns false).
  {
    MiniRun r;
    writeFile(r.dir + "/manifest.json", "{\"schema\":0}");
    snowglobe::share::VerifyResult vr;
    std::string err;
    CHECK(!snowglobe::share::verifyRun(r.dir, vr, err));
    CHECK(!err.empty());
  }
}

TEST_CASE("share verify checks blob presence", "[share]") {
  const std::string dir = tmpDir("blobs");
  snowglobe::trace::JsonlWriter w(dir + "/events.jsonl");
  REQUIRE(w.ok());
  REQUIRE(w.writeEvent(
      0, "{\"ev\":\"llm.request\",\"id\":0,\"provider\":\"openai\",\"method\":\"POST\","
         "\"path\":\"/p\",\"model\":\"m\",\"bytes\":2,\"stream\":false}"));
  REQUIRE(w.writeEvent(1, "{\"ev\":\"llm.response\",\"id\":0,\"status\":200,\"bytes\":1,"
                          "\"ttfb_ms\":null,\"total_ms\":1,\"chunk_count\":1,\"truncated\":false,"
                          "\"req\":\"llm/0000.req.json\",\"res\":\"llm/0000.res.json\","
                          "\"idx\":\"llm/0000.res.idx\"}"));
  std::ostringstream m;
  m << "{\"schema\":0,\"event_count\":2,\"last_hash\":\"" << w.lastHash() << "\"}";
  writeFile(dir + "/manifest.json", m.str());
  snowglobe::share::VerifyResult vr;
  std::string err;
  // Blobs absent -> presence failure naming the blob.
  REQUIRE(snowglobe::share::verifyRun(dir, vr, err));
  CHECK(!vr.ok);
  // Create all three + the request envelope -> clean.
  fs::create_directories(dir + "/llm");
  writeFile(dir + "/llm/0000.req.json", "{}");
  writeFile(dir + "/llm/0000.res.json", "x");
  writeFile(dir + "/llm/0000.res.idx", "");
  REQUIRE(snowglobe::share::verifyRun(dir, vr, err));
  CHECK(vr.ok);
  fs::remove_all(dir);
}

TEST_CASE("share collect skips tmp and refuses specials", "[share]") {
  // Hand-built run dir (no snowglobe invocation needed): manifest +
  // events carry no llm refs, so blob presence is vacuous here.
  const std::string dir = tmpDir("collect");
  snowglobe::trace::JsonlWriter w(dir + "/events.jsonl");
  REQUIRE(w.ok());
  REQUIRE(w.writeEvent(0, "{\"ev\":\"proc.exec\",\"path\":\"/bin/true\"}"));
  std::ostringstream m;
  m << "{\"schema\":0,\"event_count\":1,\"last_hash\":\"" << w.lastHash() << "\"}";
  writeFile(dir + "/manifest.json", m.str());
  // Torn blob debris plus an upper containing a fifo.
  fs::create_directories(dir + "/llm");
  writeFile(dir + "/llm/0000.res.json", "{}");
  writeFile(dir + "/llm/0000.res.tmp", "torn");
  fs::create_directories(dir + "/overlay/upper/sub");
  writeFile(dir + "/overlay/upper/keep.txt", "keep");
  writeFile(dir + "/overlay/upper/sub/more.txt", "more");
  REQUIRE(::mkfifo((dir + "/overlay/upper/pipe").c_str(), 0600) == 0);
  // Without overlay: upper ignored, fifo never seen -> clean collect.
  {
    std::vector<std::string> arcs, notes;
    std::string err;
    REQUIRE(snowglobe::share::collectEntries(dir, false, arcs, notes, err));
    CHECK(std::find(arcs.begin(), arcs.end(), "llm/0000.res.json") != arcs.end());
    CHECK(std::find(arcs.begin(), arcs.end(), "llm/0000.res.tmp") == arcs.end());
    CHECK(!notes.empty()); // torn skip is noted, not silent
    CHECK(std::find(arcs.begin(), arcs.end(), "overlay/upper/keep.txt") == arcs.end());
  }
  // With overlay: the fifo refuses the whole share.
  {
    std::vector<std::string> arcs, notes;
    std::string err;
    CHECK(!snowglobe::share::collectEntries(dir, true, arcs, notes, err));
    CHECK(err.find("pipe") != std::string::npos);
  }
  fs::remove_all(dir);
}

TEST_CASE("share audit scans across chunk edges", "[share]") {
  // 2 MiB file with a token straddling the 1 MiB audit window edge and
  // one fully inside the overlap: both must report with right lines.
  const std::string dir = tmpDir("audit");
  const size_t edge = 1 << 20;
  std::string big(edge + 64, 'A');
  big += "\nline2 sk-test-ABCDEF1234567890abcdef tail\n";
  big += std::string(edge, 'B');
  const std::string key = "sk-test-EDGEEDGEEDGEEDGE1234";
  big.replace(edge - 6, key.size(), key); // straddles edge-6..edge+20
  writeFile(dir + "/big.txt", big);
  std::vector<snowglobe::share::AuditHit> hits;
  std::string err;
  REQUIRE(snowglobe::share::auditEntries(dir, {"big.txt"}, hits, err));
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].file == "big.txt");
  CHECK(hits[0].kind == "sk");
  CHECK(hits[0].line == 1);
  CHECK(hits[1].line == 2);
  fs::remove_all(dir);
}

TEST_CASE("share pack unpack roundtrip is deterministic", "[share]") {
  MiniRun r;
  fs::create_directories(r.dir + "/llm");
  writeFile(r.dir + "/llm/0000.req.json", "{\"method\":\"POST\"}");
  std::vector<std::string> arcs, notes;
  std::string err;
  REQUIRE(snowglobe::share::collectEntries(r.dir, false, arcs, notes, err));
  REQUIRE(snowglobe::share::packBundle(r.dir, arcs, r.dir + ".a.sgrx", err));
  REQUIRE(snowglobe::share::packBundle(r.dir, arcs, r.dir + ".b.sgrx", err));
  const std::string ha = snowglobe::util::sha256Hex([&] {
    std::ifstream f(r.dir + ".a.sgrx", std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }());
  const std::string hb = snowglobe::util::sha256Hex([&] {
    std::ifstream f(r.dir + ".b.sgrx", std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }());
  CHECK(ha == hb);
  // Unpack and re-verify through the bundle path.
  REQUIRE(snowglobe::share::extractBundle(r.dir + ".a.sgrx", r.dir + ".x", err));
  snowglobe::share::VerifyResult vr;
  REQUIRE(snowglobe::share::verifyRun(r.dir + ".x", vr, err));
  CHECK(vr.ok);
  CHECK(vr.events == 2);
  snowglobe::share::VerifyResult vb;
  REQUIRE(snowglobe::share::verifyBundle(r.dir + ".a.sgrx", r.dir + ".y", vb, err));
  CHECK(vb.ok);
  fs::remove_all(r.dir + ".x");
  fs::remove_all(r.dir + ".y");
  fs::remove_all(r.dir + ".a.sgrx");
  fs::remove_all(r.dir + ".b.sgrx");
}

TEST_CASE("share fixtures audit clean",
          "[share]") { // Real recorded shapes (100 KB repeated system prompts included) must
  // not trip the audit: pins no-false-positives where it matters.
  const std::string fix = SNOWGLOBE_FIXTURES_DIR;
  for (const char* fx : {"toy-agent-3turn", "claude-code-1-error"}) {
    const std::string root = fix + "/" + fx;
    std::vector<std::string> arcs;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator();
         ++it) {
      if (it->is_regular_file()) {
        arcs.push_back(fs::relative(it->path(), root).generic_string());
      }
    }
    std::vector<snowglobe::share::AuditHit> hits;
    std::string err;
    REQUIRE(snowglobe::share::auditEntries(root, arcs, hits, err));
    CHECK(hits.empty());
  }
}

namespace {
void writeBundle(const std::string& path, const std::string& tarBytes) {
  const size_t bound = ZSTD_compressBound(tarBytes.size());
  std::string out(bound, '\0');
  const size_t done = ZSTD_compress(out.data(), bound, tarBytes.data(), tarBytes.size(), 3);
  REQUIRE(!ZSTD_isError(done));
  out.resize(done);
  writeFile(path, out);
}
} // namespace

TEST_CASE("share extract refuses symlink traversal", "[share]") {
  // D1 (Medium): a crafted bundle with symlink `a` plus file `a/b`
  // would write outside dest when create_directories follows the link.
  const std::string dir = tmpDir("traverse");
  TarEntry link, child, solo;
  link.name = "a";
  link.isSymlink = true;
  link.linkTarget = "/tmp/sg-share-should-not-exist";
  child.name = "a/b";
  child.data = "escape";
  solo.name = "ok.txt";
  solo.data = "fine";
  std::string tarBytes, err;
  REQUIRE(snowglobe::share::tarWrite({link, child}, tarBytes, err));
  writeBundle(dir + "/evil.sgrx", tarBytes);
  std::string xerr;
  CHECK(!snowglobe::share::extractBundle(dir + "/evil.sgrx", dir + "/x", xerr));
  CHECK(xerr.find("traverses") != std::string::npos);
  CHECK(!fs::exists(dir + "/x/a"));
  // Benign control: lone symlink extracts fine.
  REQUIRE(snowglobe::share::tarWrite({link, solo}, tarBytes, err));
  writeBundle(dir + "/ok.sgrx", tarBytes);
  REQUIRE(snowglobe::share::extractBundle(dir + "/ok.sgrx", dir + "/y", xerr));
  CHECK(fs::is_symlink(dir + "/y/a"));
  CHECK(!fs::exists("/tmp/sg-share-should-not-exist"));
  fs::remove_all(dir);
  fs::remove_all("/tmp/sg-share-should-not-exist");
}

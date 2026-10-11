// Minimal ustar tar writer/reader (ADR-0011).
#include "tar.hpp"

#include <cstdio>
#include <cstring>

namespace snowglobe::share {
namespace {

// ustar header layout (all ASCII, NUL-padded, octal numbers).
constexpr size_t kBlock = 512;
constexpr size_t kNameOff = 0, kNameLen = 100;
constexpr size_t kModeOff = 100, kModeLen = 8;
constexpr size_t kUidOff = 108, kUidLen = 8;
constexpr size_t kGidOff = 116, kGidLen = 8;
constexpr size_t kSizeOff = 124, kSizeLen = 12;
constexpr size_t kMtimeOff = 136, kMtimeLen = 12;
constexpr size_t kChksumOff = 148, kChksumLen = 8;
constexpr size_t kTypeOff = 156;
constexpr size_t kLinkOff = 157, kLinkLen = 100;
constexpr size_t kMagicOff = 257;
constexpr size_t kPrefixOff = 345, kPrefixLen = 155;

void putOctal(char* dst, size_t len, uint64_t v, bool& ok) {
  // len includes the trailing NUL. Manual digits (no snprintf): the
  // -Wformat-truncation MAY-warning fires on the printf form under -O3
  // cross compilers, and a manual loop turns silent truncation into a
  // loud overflow refusal instead (a >8 GiB file must fail, never wrap).
  ok = true;
  for (size_t k = len - 1; k-- > 0;) {
    dst[k] = static_cast<char>('0' + (v & 7));
    v >>= 3;
  }
  dst[len - 1] = '\0';
  if (v != 0) {
    ok = false;
  }
}

bool getOctal(const char* src, size_t len, uint64_t& v) {
  // Tolerate NUL- or space-terminated fields (writers vary).
  char buf[32] = {};
  if (len >= sizeof(buf)) {
    return false;
  }
  std::memcpy(buf, src, len);
  buf[len] = '\0';
  // GNU base-256 (high bit set) is out of scope: our writer never emits
  // it and bundles only carry small files (loud reject).
  if ((static_cast<unsigned char>(buf[0]) & 0x80) != 0) {
    return false;
  }
  size_t i = 0;
  while (i < len && (buf[i] == ' ' || buf[i] == '\0')) {
    ++i;
  }
  if (i >= len) {
    v = 0;
    return true;
  }
  uint64_t acc = 0;
  size_t digits = 0;
  while (i < len && buf[i] >= '0' && buf[i] <= '7') {
    acc = acc * 8 + static_cast<uint64_t>(buf[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 0) {
    return false;
  }
  v = acc;
  return true;
}

// Split an archive name into prefix + name (ustar allows 155 + 100).
// False when unrepresentable (caller fails loud, never truncates).
bool splitName(const std::string& name, std::string& prefix, std::string& base) {
  if (name.size() <= kNameLen) {
    prefix.clear();
    base = name;
    return true;
  }
  // Longest fitting tail first: smallest cut with tail <= 100.
  for (size_t cut = name.size() - kNameLen; cut <= name.size(); ++cut) {
    if (cut < 2 || name[cut - 1] != '/') {
      continue;
    }
    const std::string pre = name.substr(0, cut - 1);
    const std::string bs = name.substr(cut);
    if (!pre.empty() && pre.size() <= kPrefixLen && !bs.empty() && bs.size() <= kNameLen) {
      prefix = pre;
      base = bs;
      return true;
    }
  }
  return false;
}

bool badName(const std::string& name) {
  if (name.empty() || name[0] == '/') {
    return true;
  }
  size_t i = 0;
  while (i <= name.size()) {
    size_t j = name.find('/', i);
    const std::string seg = (j == std::string::npos) ? name.substr(i) : name.substr(i, j - i);
    if (seg == "..") {
      return true;
    }
    if (j == std::string::npos) {
      break;
    }
    i = j + 1;
  }
  return false;
}

} // namespace

bool tarWrite(const std::vector<TarEntry>& entries, std::string& out, std::string& err) {
  out.clear();
  for (const TarEntry& e : entries) {
    if (badName(e.name)) {
      err = "tar: refusing absolute/parent-escape name '" + e.name + "'";
      return false;
    }
    std::string prefix, base;
    if (!splitName(e.name, prefix, base)) {
      err = "tar: name too long for ustar '" + e.name + "'";
      return false;
    }
    if (e.isDir && e.isSymlink) {
      err = "tar: entry is both dir and symlink '" + e.name + "'";
      return false;
    }
    if (e.isSymlink && e.linkTarget.size() > kLinkLen) {
      err = "tar: symlink target too long '" + e.name + "'";
      return false;
    }
    char h[kBlock] = {};
    std::memcpy(h + kNameOff, base.data(), base.size());
    // Normalized metadata (ADR-0011): fixed mtime/uid/gid/names. File
    // modes preserved (informative); dirs always 0755.
    bool okNum = true;
    putOctal(h + kModeOff, kModeLen, e.isDir ? 0755 : (e.mode & 07777), okNum);
    putOctal(h + kUidOff, kUidLen, 0, okNum);
    putOctal(h + kGidOff, kGidLen, 0, okNum);
    putOctal(h + kSizeOff, kSizeLen, e.isDir ? 0 : e.data.size(), okNum);
    putOctal(h + kMtimeOff, kMtimeLen, 0, okNum);
    if (!okNum) {
      err = "tar: numeric overflow for '" + e.name + "'";
      return false;
    }
    h[kTypeOff] = e.isDir ? '5' : (e.isSymlink ? '2' : '0');
    if (e.isSymlink) {
      std::memcpy(h + kLinkOff, e.linkTarget.data(), e.linkTarget.size());
    }
    std::memcpy(h + kMagicOff, "ustar", 5);
    h[kMagicOff + 5] = '\0';
    h[kMagicOff + 6] = '0';
    h[kMagicOff + 7] = '0';
    if (!prefix.empty()) {
      std::memcpy(h + kPrefixOff, prefix.data(), prefix.size());
    }
    // Checksum over the header with the checksum field as spaces.
    std::memset(h + kChksumOff, ' ', kChksumLen);
    unsigned sum = 0;
    for (char c : h) {
      sum += static_cast<unsigned char>(c);
    }
    bool okSum = true;
    putOctal(h + kChksumOff, kChksumLen - 1, sum, okSum);
    h[kChksumOff + 6] = '\0';
    h[kChksumOff + 7] = ' ';
    if (!okSum) {
      err = "tar: checksum overflow (unreachable)";
      return false;
    }
    out.append(h, kBlock);
    if (!e.isDir && !e.isSymlink && !e.data.empty()) {
      out.append(e.data);
      const size_t pad = (kBlock - (e.data.size() % kBlock)) % kBlock;
      out.append(pad, '\0');
    }
  }
  out.append(2 * kBlock, '\0'); // end-of-archive marker
  return true;
}

bool tarRead(const std::string& blob, std::vector<TarEntry>& entries, std::string& err) {
  entries.clear();
  size_t off = 0;
  const size_t n = blob.size();
  long emptyRun = 0;
  while (off + kBlock <= n) {
    const char* h = blob.data() + off;
    bool allZero = true;
    for (size_t k = 0; k < kBlock; ++k) {
      if (h[k] != '\0') {
        allZero = false;
        break;
      }
    }
    if (allZero) {
      if (++emptyRun >= 2) {
        break; // clean end of archive
      }
      off += kBlock;
      continue;
    }
    emptyRun = 0;
    if (std::memcmp(h + kMagicOff, "ustar", 5) != 0) {
      err = "tar: bad magic at block " + std::to_string(off / kBlock);
      return false;
    }
    unsigned want = 0;
    {
      char tmp[kChksumLen] = {};
      std::memcpy(tmp, h + kChksumOff, kChksumLen);
      // Checksum field parses as octal with trailing NUL/space.
      uint64_t v = 0;
      if (!getOctal(tmp, kChksumLen, v)) {
        err = "tar: bad checksum field at block " + std::to_string(off / kBlock);
        return false;
      }
      want = static_cast<unsigned>(v);
    }
    unsigned got = 0;
    for (size_t k = 0; k < kBlock; ++k) {
      const char c = (k >= kChksumOff && k < kChksumOff + kChksumLen) ? ' ' : h[k];
      got += static_cast<unsigned char>(c);
    }
    if (got != want) {
      err = "tar: checksum mismatch at block " + std::to_string(off / kBlock);
      return false;
    }
    std::string base(h + kNameOff, strnlen(h + kNameOff, kNameLen));
    std::string prefix(h + kPrefixOff, strnlen(h + kPrefixOff, kPrefixLen));
    const std::string name = prefix.empty() ? base : prefix + "/" + base;
    if (badName(name)) {
      err = "tar: refusing absolute/parent-escape name '" + name + "'";
      return false;
    }
    uint64_t size = 0, mode = 0;
    if (!getOctal(h + kSizeOff, kSizeLen, size) || !getOctal(h + kModeOff, kModeLen, mode)) {
      err = "tar: bad size/mode at block " + std::to_string(off / kBlock);
      return false;
    }
    const char type = h[kTypeOff];
    TarEntry e;
    e.name = name;
    e.mode = static_cast<unsigned>(mode & 07777);
    if (type == '5') {
      e.isDir = true;
    } else if (type == '2') {
      e.isSymlink = true;
      e.linkTarget.assign(h + kLinkOff, strnlen(h + kLinkOff, kLinkLen));
    } else if (type == '0' || type == '\0') {
      // regular file
    } else {
      err = "tar: unsupported typeflag '" + std::string(1, type) + "' for '" + name + "'";
      return false;
    }
    off += kBlock;
    if (!e.isDir && !e.isSymlink) {
      if (size > n - off) {
        err = "tar: truncated data for '" + name + "'";
        return false;
      }
      e.data.assign(blob.data() + off, static_cast<size_t>(size));
      off += (static_cast<size_t>(size) + kBlock - 1) / kBlock * kBlock;
    }
    entries.push_back(std::move(e));
  }
  if (off > n) {
    err = "tar: trailing garbage";
    return false;
  }
  return true;
}

} // namespace snowglobe::share

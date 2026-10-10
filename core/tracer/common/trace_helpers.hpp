#pragma once
// Shared tracee-memory/path helpers for both tracer backends (ADR-0009).
// Pure functions over (pid, address): the ptrace backend calls them at
// syscall ENTRY/EXIT stops, the notify backend at notification time.
// One implementation so path canonicalisation, string reads and the
// default path filters are identical by construction — parity by
// construction for everything entry-known. Thread ownership: stateless
// free functions, safe from any thread.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

namespace snowglobe::tracer {

std::string errnoText(int e);

// x32 ABI high bit (R5 class): x32 syscalls reuse the native audit arch
// with bit 30 set in nr. stripX32Nr maps them back to the native number
// so both backends classify x32 identically to native (identity for
// native nrs, which never reach the bit). Used by the ptrace ENTRY path
// and the notify decode path; the notify BPF traps the high range so
// x32 stays observed, never invisible.
constexpr uint64_t kX32Bit = 0x40000000U;

inline uint64_t stripX32Nr(uint64_t nr) {
  return nr & ~kX32Bit;
}

uint64_t clockUs(int clk);

// Read up to maxLen bytes from remote address; bytes read or -1.
long vmRead(int pid, uint64_t remote, char* out, std::size_t maxLen);

// Read a NUL-terminated string from the tracee (up to 4096+256 bytes).
// False + errDetail (never silent): "null pointer", "process_vm_readv:
// ...", "string too long or unterminated".
bool vmReadStr(int pid, uint64_t remote, std::string& out, std::string& errDetail);

bool vmReadU64(int pid, uint64_t remote, uint64_t& out);

std::string readLink(const std::string& path);

// Resolve a tid's thread-group id via /proc. Leaders resolve to
// themselves; unreadable (already gone) resolves to tid — tid
// attribution survives, the tgid may be approximate for flash-lived
// threads.
int threadGroupId(int tid);

// Normalise an absolute path lexically (no filesystem access).
std::string normaliseAbs(const std::string& p);

// Default noisy-path filter (mirrors docs/trace-format.md §filtering:
// proc/sys/dev + loader/locale noise).
bool isNoisyPath(const std::string& p);

// Resolve raw against the tracee's cwd (absolute as-is) or dirfd:
// dirfd != AT_FDCWD resolves against /proc/PID/fd/N, falling back to
// cwd when the fd link is unreadable. Lexically normalised.
std::string canonicalPath(int pid, long dirfd, const std::string& raw);

// Read an execve/execveat path+argv from the tracee *now* (valid only
// while the calling image is still mapped — i.e. at ENTRY, at EXIT on
// failure, or at seccomp-notify time). truncatedOut is set when argv was
// cut (64-entry cap or mid-array read failure): callers report
// truncated:true rather than dropping silently. Every argv element is
// secret-redacted before escaping.
bool readExecStrings(int pid, long dirfd, uint64_t pathAddr, uint64_t argvAddr,
                     const std::vector<std::pair<std::string, std::string>>& secretEnv,
                     std::string& canonOut, std::string& argvJsonOut, std::string& detailOut,
                     bool& truncatedOut);

} // namespace snowglobe::tracer

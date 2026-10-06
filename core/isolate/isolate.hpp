#pragma once
// Unprivileged isolation for `snowglobe run --isolate` (ADR-0007).
// Containment of accidents, never a security boundary: no seccomp,
// landlock, env masking or netns yet (Block 2); host network stays.
//
// Shape: the tracer forks as usual; the forked middle (same code path,
// same first SIGSTOP) enters user+mount namespaces (parent-written id
// maps — self-map is EPERM on some kernels), builds a selective root
// (overlayfs on the project dir + /etc, read-only binds elsewhere,
// fresh procfs, tmpfs /tmp), pivot_roots, unshares a pid namespace,
// then forks a PID-1 init-helper that reaps and propagates the agent's
// exit code. The tracer stays outside every namespace, unchanged.
//
// Thread ownership: supervisor calls run in its thread; enterChild runs
// in the forked tracee (async-signal-adjacent: only async-safe effects
// plus _exit; stdio never used on failure paths — status pipe only).
#include <string>
#include <vector>

namespace snowglobe::isolate {

// Absolute overlay backing dirs under <run>/overlay (created pre-fork).
struct OverlayDirs {
  std::string upper;    // project overlay upper (diff/apply source)
  std::string work;     // project overlay workdir (must start empty)
  std::string etcUpper; // /etc overlay upper (system writes, not for apply)
  std::string etcWork;  // /etc overlay workdir
  std::string mnt;      // tmpfs base; the new root lives at mnt itself
};

// Supervisor side, pre-fork: mkdir overlay backing dirs. False + error.
bool prepareRunDir(const std::string& runDir, OverlayDirs& dirs, std::string& error);

// Pipe ends for the two handshakes (all O_CLOEXEC via pipe2).
struct ChildPipes {
  int mapReqR = -1; // middle->supervisor "M" (supervisor keeps R)
  int mapReqW = -1; // (middle keeps W)
  int mapAckR = -1; // (middle keeps R)
  int mapAckW = -1; // supervisor keeps W
  int statusR = -1; // middle->supervisor "ok"|"ERR msg" (supervisor keeps R)
  int statusW = -1; // (middle keeps W)
};
bool makePipes(ChildPipes& p, std::string& error);
void closeSupervisorEnds(ChildPipes& p); // middle calls after fork
void closeMiddleEnds(ChildPipes& p);     // supervisor calls after fork

// Supervisor side, post-fork: serve one map request, then read setup
// status. False + error names step + errno (caller exits 69). Reaps the
// middle on the failure paths; on success the middle is left stopped at
// its first SIGSTOP for the normal tracer loop.
bool serveMaps(pid_t child, ChildPipes& p, std::string& error);
bool awaitReady(pid_t child, ChildPipes& p, std::string& error);

struct ChildConfig {
  std::string projectDir; // absolute repo path (validated pre-fork, != /)
  OverlayDirs dirs;       // absolute host paths
  std::vector<std::string> cmd;
  int mapReqW = -1;
  int mapAckR = -1;
  int statusW = -1;
};

// Tracee-child side: full setup through pivot_root, then PTRACE_TRACEME
// + SIGSTOP (preserving the tracer's first-stop contract), fork of the
// PID-1 init-helper, wait loop, _exit with the agent's code. Never
// returns on success; on setup failure writes "ERR ..." and _exits 70.
void enterChild(const ChildConfig& cfg);

} // namespace snowglobe::isolate

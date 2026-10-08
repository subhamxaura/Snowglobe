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

#ifdef __linux__
#include <sys/types.h>
#endif

namespace snowglobe::isolate {

// Absolute overlay backing dirs under <run>/overlay (created pre-fork).
struct OverlayDirs {
  std::string upper;     // project overlay upper (diff/apply source)
  std::string work;      // project overlay workdir (must start empty)
  std::string etcUpper;  // /etc overlay upper (system writes, not for apply)
  std::string etcWork;   // /etc overlay workdir
  std::string homeUpper; // $HOME overlay upper (agent state, not for apply)
  std::string homeWork;  // $HOME overlay workdir
  std::string mnt;       // tmpfs base; the new root lives at mnt itself
};

// Extra --fs-rw PATH (repeatable): overlay-mounted read-write.
struct FsRwMount {
  std::string path;  // absolute target (must exist, must be a dir)
  std::string upper; // absolute backing dir
  std::string work;  // absolute backing dir
};

// Supervisor side, pre-fork: create <run>/overlay backing dirs (fixed set
// plus one upper/work pair per fsRw path). False + error.
bool prepareRunDir(const std::string& runDir, const std::vector<std::string>& fsRwPaths,
                   OverlayDirs& dirs, std::vector<FsRwMount>& fsRw, std::string& error);

// Pipe ends for the two handshakes (all O_CLOEXEC via pipe2).
struct ChildPipes {
  int mapReqR = -1; // middle->supervisor "M" (supervisor keeps R)
  int mapReqW = -1; // (middle keeps W)
  int mapAckR = -1; // (middle keeps R)
  int mapAckW = -1; // supervisor keeps W
  int statusR = -1; // middle->supervisor "ok"|"ERR msg" (supervisor keeps R)
  int statusW = -1; // (middle keeps W)
  int envR = -1;    // supervisor->middle injected env block (middle keeps R)
  int envW = -1;    // (supervisor keeps W)
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

// Supervisor side, post proxy-start: forward proxy-injected env
// ("K=V" lines + "END") to the waiting middle. Always called under
// --isolate (possibly an empty block); the middle blocks reading it,
// so a missing call hangs the run — never skip it.
bool writeEnvBlock(int envW, const std::vector<std::pair<std::string, std::string>>& env,
                   std::string& error);

struct ChildConfig {
  std::string projectDir; // absolute repo path (validated pre-fork, != /)
  std::string homeDir;    // absolute $HOME (validated pre-fork)
  OverlayDirs dirs;       // absolute host paths
  std::vector<FsRwMount> fsRw;
  std::vector<std::string> allowEnv;  // re-admitted secret names (exact)
  std::vector<std::string> allowPath; // --allow-path exemptions (absolute, exact or parent)
  std::vector<std::string> cmd;
  // prlimit fallback (applied in the agent child just before exec, never
  // in middle/init — an address limit would kill instrumented observers;
  // cgroup adds on top when delegated): NPROC+NOFILE always, AS only when
  // memExplicit (AS breaks Bun).
  long long pidsMax = 512;    // RLIMIT_NPROC
  long long nofileMax = 1024; // RLIMIT_NOFILE
  long long memBytes = -1;    // RLIMIT_AS bytes, -1 = no AS limit
  bool memExplicit = false;   // true iff --memory-max was given explicitly
  int mapReqW = -1;
  int mapAckR = -1;
  int statusW = -1;
  int envR = -1; // injected-env block reader ("K=V" lines + "END")
};

// Default secret-path masks under $HOME (empty-tmpfs, 0700): .ssh, .aws,
// .gnupg. Returned as absolute paths (homeDir + suffix). Exemptions via
// allowPath (exact match or parent) are removed.
std::vector<std::string> defaultSecretMasks(const std::string& homeDir,
                                            const std::vector<std::string>& allowPath);

// Fork the middle; call ONLY while still single-threaded (before the LLM
// proxy pool exists). A fork from a multithreaded process inherits
// userspace locks (e.g. the sanitizer allocator) held at fork instant and
// deadlocks the child on first use — proven in-suite. In the child closes
// supervisor ends and runs enterChild (noreturn); in the parent closes
// middle ends and returns the pid, or -1 + error.
pid_t spawnMiddle(const ChildConfig& cfg, ChildPipes& pipes, std::string& error);

// True when an env NAME is secret-shaped (redact module's name rule:
// (KEY|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIAL)$ case-insensitive).
bool isSecretName(const std::string& name);

// Strip secret-named vars from this process's environ, except allow-listed
// (exact match) and *_BASE_URL (proxy wiring — always passes).
void stripSecretEnv(const std::vector<std::string>& allowEnv);

// Parse a byte size (plain int, K/M/G suffix, "max" = -1). False + error.
bool parseMemSize(const std::string& s, long long& bytes, std::string& error);

// Best-effort cgroup v2 placement for childPid: <parent>/sg-<tag> with
// memory.max + pids.max. NEVER fatal: false + note explains (delegation
// denied, no v2, ...), caller proceeds without limits. outPath is set iff
// a cgroup was created (caller rmdirs it best-effort at the end).
bool joinCgroup(const std::string& tag, int childPid, long long memBytes, long long pidsMax,
                std::string& note, std::string& outPath);

// Tracee-child side: full setup through pivot_root, then PTRACE_TRACEME
// + SIGSTOP (preserving the tracer's first-stop contract), fork of the
// PID-1 init-helper, wait loop, _exit with the agent's code. Never
// returns on success; on setup failure writes "ERR ..." and _exits 70.
void enterChild(const ChildConfig& cfg);

} // namespace snowglobe::isolate

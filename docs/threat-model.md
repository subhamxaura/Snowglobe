# Threat model — what `--isolate` is and is NOT

Date: 2026-10-08 · Status: accepted · Phase: 3 ITEM 0 (fulfils the
AGENTS.md §1.4 promise: "hostile code belongs in a microVM (document in
`docs/threat-model.md`)").

## Framing

Snowglobe is **isolation and visibility**, never a security boundary for
hostile code. `snowglobe run --isolate` contains *accidents* by
non-hostile agents (a coding agent `rm -rf`ing the wrong directory,
writing credentials into a log, wandering out of the repo). It does not
make untrusted code safe to run. If the code or the agent may be
malicious, run it in a microVM (or equivalent) — not under `--isolate`.

## What `--isolate` is

Accident containment plus complete observation, all unprivileged
(mechanism: ADR-0007):

- The agent runs in user+mount+pid namespaces with the project dir on
  overlayfs (writes land in `<run>/overlay/upper`, the host tree stays
  clean), host `/` read-only, `/tmp` tmpfs, empty-tmpfs `/proc`.
- Secret paths (`~/.ssh`, `~/.aws`, `~/.gnupg`) and secret-named env
  vars are masked/stripped by default (`--allow-path` / `--allow-env`
  re-admit explicitly, recorded in the manifest).
- A seccomp-bpf deny-list (Docker-default parity + documented strict
  supersets) and Landlock (RO world, RW islands) refuse dangerous calls
  loudly; cgroup/prlimit bounds resources best-effort.
- The ptrace tracer stays *outside* every namespace and records every
  decoded side effect into a hash-chained trace, so accidents are
  reviewable (`diff`) before anything reaches the host (`apply`).

## What `--isolate` is NOT (explicit non-goals)

- **Not a sandbox for hostile code.** A kernel exploit, a container
  escape, or a deliberately malicious agent is out of scope. No
  bounty, no CVE-grade claims, no audit has been performed.
- **No network confinement this phase.** Host network stays (no netns;
  `pasta` absent): the agent can open arbitrary egress. Egress is
  *observed* (`net.connect`/`net.dns`), not prevented.
- **No protection against exfiltration through allowed channels.** The
  agent can legitimately write the repo, `/tmp`, `$HOME` state, and the
  network; anything it can read there it can transmit. Masking covers
  only the default secret paths, not every secret-shaped byte.
- **No resource-hard guarantees.** cgroup limits apply only with
  delegation, otherwise `prlimit` (which the agent cannot raise but a
  fork bomb can still approach). Treat limits as hygiene, not a DoS
  boundary.
- **No integrity of the lower layers.** The read-only bind and overlays
  assume a non-adversarial agent; confused-deputy writes outside the
  project fail loudly (EROFS) but are not a proved-confinement claim.
- **No side-channel or covert-channel analysis.** Timing, cache, and
  storage channels between sandbox and host are unmodelled.

## Boundary table

| Threat | Handled? | By what |
|---|---|---|
| Agent writes outside the repo by mistake | Yes (contained) | overlayfs upper + ro bind + Landlock; reviewed via `diff` |
| Agent reads `~/.ssh` / secrets env by mistake | Yes (masked) | tmpfs masks + env stripping; `--allow-path` is explicit |
| Agent forks/threads normally | Yes (allowed) | clone without namespace flags passes the filter |
| Agent creates namespaces, mounts, reboots, loads modules | Refused loudly | seccomp EPERM / KILL + Landlock; traced |
| Agent needs `/proc/self` (Bun/Node) | Degraded (known) | empty-tmpfs `/proc`; issue #4; opt-in host rbind under discussion only |
| Malicious agent or kernel exploit | **No** | microVM required |
| Network exfiltration / SSRF | Observed, not blocked | `net.*` events; `--net` confinement is future work |
| Resource exhaustion as attack | Best-effort only | cgroup/prlimit hygiene |

## Terminology rule

Never use the prohibited two-word term for it — not in code, docs, or
output (the single allowed occurrence is the prohibition line in
AGENTS.md §1.4 itself; this file avoids repeating the exact bigram on
purpose so that check stays at 1). The words are "isolation and
visibility". Fixture blobs embed the constitution verbatim and are
excluded from that check.

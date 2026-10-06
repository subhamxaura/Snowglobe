# ADR 0007 — unprivileged isolation stack for `run --isolate`

Date: 2026-10-05 · Status: accepted · Phase: 2 Block 1

## Context

`snowglobe run --isolate` must contain agent side effects (accidents, not
adversaries — never "secure sandbox") with no root and no Docker: user +
mount + pid namespaces, overlayfs, pivot into a new root, PID-1 reaping.
The constitution (§1.4) prescribes project-dir overlay + host `/` ro +
tmpfs `/tmp`. This ADR amends the CLI contract (AGENTS.md §2) with the
`--isolate` flag.

## Probes (all on WSL2 6.6.87, uid 1000; repro in probe/)

- **ID maps are parent-written.** Self `uid_map` write: EPERM
  (probe/overlayfs_userns.c, doctor row). Parent single-id map after the
  child unshares ("0 <uid> 1", setgroups-deny first): works
  (probe/map_parent.c — the `unshare --map-user` rule). The middle
  therefore requests maps over a pipe; the supervisor writes them.
- **`lowerdir=/` is impossible here.** Overlay mount: EINVAL, dmesg
  "failed to clone lowerpath". `lowerdir=/home` (clean ext4): OK.
  `lowerdir=/usr`: EINVAL (poisoned by nested overlay + 9p submounts).
  The task text's `lowerdir=/` is abandoned with this evidence; the
  constitution's project-dir overlay stands.
- **Tracing across the boundary needs no tracer change.** A pidns
  middle/init/agent tree under `snowglobe run` shows every proc.start,
  exec and exit code through two reaping layers
  (probe/ns_harness.c); the tracer already waits with `__WALL`.
- **No fresh procfs on this kernel.** New proc/sysfs instances inside a
  userns: EPERM (tmpfs mounts work — full caps present, so this is
  kernel policy, not missing privilege). Merged `/proc` is therefore an
  empty tmpfs: host pids stay invisible either way, but agents needing
  `/proc` introspection degrade. Loud limitation, revisited if kernels
  allow it.

## Decision

- Three layers: traced middle (namespaces, mounts, pivot) → PID-1
  init-helper (reaps, propagates the agent's exit code) → agent.
  Exit codes propagate unchanged through both reapers.
- Selective mounts: overlayfs on the **project dir**
  (upper `<run>/overlay/upper`, the Phase-3 diff/apply source) and on
  **/etc** (upper `<run>/overlay/system-upper`, not for diff/apply);
  everything else read-only bind (symlinks recreated), empty-tmpfs
  `/proc` (fresh procfs denied, see probes), tmpfs `/tmp` + `/run`,
  pivot_root (failure: exit 69, never chroot fallback). Writes outside
  repo/etc/tmp fail EROFS — loudly traced, by design.
- Ancestor shadowing (observed): repo ancestors resolve empty except
  the repo chain (e.g. project under /mnt hides the /mnt rbind) —
  containment-consistent; sibling trees are simply not there.
- `run.meta` gains `"isolate":true` (additive, schema stays 0);
  manifest `isolate:{"on":true,"features":["userns","mount","pid",
  "overlay"],"upper":"overlay/upper"}`. Non-isolate output is
  byte-identical to before (goldens prove it).
- Any setup failure names step + errno and exits 69 (missing
  capability, never silent). A ptrace failure across the boundary
  (first stop never arrives) is exit 69, never a weakened tracer.

## Consequences

- Host network stays (no netns this phase — pasta absent; limitation
  documented loudly in Block 3).
- No seccomp/landlock/env-masking/cgroup yet (Block 2); no adversarial
  claims anywhere (grep "secure sandbox" must stay 0).
- `doctor`'s overlayfs-in-userns check uses the same parent-map dance,
  so the gate is honest: green here, red-with-reason elsewhere.

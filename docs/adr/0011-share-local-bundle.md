# ADR 0011 — share is a local tamper-evident bundle (Phase 6 Block 1)

Date: 2026-10-10 · Status: accepted · Phase: 6 Block 1

## Context

AGENTS.md §2 sketched `share <run> [--public|--team]` as a cloud
upload (commercial Phase 5). Launch readiness needs the offline half
first: a single-file export of a run that a recipient can integrity-
check and open, with secrets structurally barred from leaving the box.
The trace is a `.sgr` directory; the bundle must be byte-deterministic
so two exports of one run compare equal.

This ADR amends the CLI contract (AGENTS.md §2): `share` becomes local
bundle export, and adds `snowglobe verify <run-or-bundle>`. Cloud
upload (`--public|--team`) stays future work.

## Decision

- **`share <run> [--out FILE.sgrx] [--force] [--with-overlay]`** writes
  a `tar+zstd` bundle (default `<run>.sgrx`, i.e. the run path plus
  `x`). Contents, in sorted order: `manifest.json`, `events.jsonl`,
  `llm/` (if present), `links.json`, `replay-report.json`,
  `baseline.json`, `fs/summary.json` (each if present except the first
  two, which are required — else 69). `overlay/upper/` is EXCLUDED by
  default and included only with `--with-overlay` (project upper only;
  etc-upper/home-upper/fs-rw-N never ship — host-shaped, not review).
- **Format, hand-rolled + locked dep only.** ustar writer/reader in
  `core/share` (no new dep): normalized metadata (mtime 0, uid/gid 0,
  empty uname/gname, sorted entries, explicit dir entries) so the tar
  bytes are a pure function of the content. zstd level 3,
  single-threaded one-shot (multithreaded zstd is nondeterministic);
  zstd comes via FetchContent pinned `v1.5.7` — hermetic on every leg
  including the aarch64 cross (no apt, no sudo anywhere). Same run
  shared twice → identical sha256 (pinned by test).
- **Gates before pack, in this order:** (1) secret audit —
  `redact::auditSecrets` (new, shape-only sibling of `redactText` with
  identical floors: Bearer/Basic ≥8, sk- ≥20, sk-ant-, ghp_/gho_/
  github_pat_ ≥20, AKIA+16, xox[abp]-, key=/token=/password= params)
  over every file that would enter the bundle; any finding refuses
  with exit 65 listing `file:line`, nothing written; (2) integrity —
  the same chain walk + manifest consistency + blob presence that
  `verify` runs. `--force` overrides (1) with a loud warning, never
  (2): a corrupt chain is tamper, not policy.
- **`verify <run-or-bundle>`**: hash-chain walk (genesis `prev_hash`
  `"0"`, seq 0..N-1, `H == sha256hex(prev + withSeq)` recomputed per
  line, tail-anchored parse, no JSON lib), manifest consistency
  (`event_count`, `last_hash`), blob presence (every `llm.response`
  req/res/idx exists and is confined; every `llm.request` id has its
  `llm/%04d.req.json`). Bundles extract to a temp dir first, then
  verify as a run. Exit 0 clean / 65 FAILED with reasons / 69
  no-such-run / 70 internal. Verify does NOT audit (integrity only —
  a `--force`d bundle with a planted key verifies fine, by design).
- **Exit codes:** share 0 packed / 64 usage / 65 refused (audit or
  integrity) / 69 no-such-run / 70 internal; verify 0 / 65 / 69 / 70.

## Consequences

- Trace `schema` stays 0 (no format change — the bundle wraps v0).
- `view` opens an extracted bundle directory unchanged (pinned e2e).
- The committed fixtures stay shareable-proof by audit-unit (toy +
  error fixtures must audit clean — pins no-false-positives on real
  shapes); the toy happy-path test pins end-to-end.
- Second-model review target for Block 3 D1 is `core/share/` with
  emphasis on the audit-before-pack path (per the Phase 6 brief).

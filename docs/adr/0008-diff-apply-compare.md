# ADR 0008 — diff/apply/compare over the overlay upper (Phase 3)

Date: 2026-10-08 · Status: accepted · Phase: 3 Blocks 1–2

## Context

`run --isolate` leaves agent writes in `<run>/overlay/upper` (project),
plus `etc-upper`, `home-upper`, `fs-rw-N` (ADR-0007). Record → Inspect →
Diff → Apply closes the loop: review the change set before anything
reaches the host tree. This ADR amends the CLI contract (AGENTS.md §2):
it activates `diff`/`apply`/`compare` (previously "not yet implemented"
stubs), adds `run --baseline-exclude`, and adds apply exit 65.

No new dependency: the unified diff is hand-rolled (Hirschberg LCS,
linear space, `core/diff/myers.*`); AGENTS.md §1.2 stays untouched. JSON
stays hand-rolled (writer + a strict reader for our own byte-exact format).

## Decision

- **baseline.json.** At `--isolate` start (supervisor, pre-fork) walk the
  project dir and write `<run>/baseline.json`:
  `{"version":1,"project":"<abs>","files":{"rel":{"type":"file","sha256":..,"size":N,"mtime":T}
  | {"type":"link","target":..}}}` (keys sorted; rel uses `/`). Skips:
  `.git/`, `.snowglobe/`, `node_modules/` by default, plus repeatable
  `run --baseline-exclude REL` (relative prefix or exact file; without
  `--isolate` it is a usage error like the other isolate-only flags).
  The effective set is stored in baseline.json (`"excludes"`) so review
  reproduces the classification exactly. The project upper walk replays
  the same set — except `.git`, which is always reviewed anyway (repo
  metadata tamper must be visible; `apply` refuses it, see below).
  Only regular files and symlinks are recorded (fifos/sockets/devices:
  skipped with a `skipped_special` count). Manifest gains additive
  `"baseline":"baseline.json"` on isolate runs (schema stays 0).
- **Change set.** `diff` walks every upper. Entries are keyed
  `(scope, relpath)`, scope ∈ project/etc/home/fs-rw-N. Project entries
  classify against baseline.json: absent→A, hash differs→M, whiteout
  (char-0/0 overlay whiteout, incl. opaque-dir handling)→D; a D+A pair
  in one scope with equal non-empty content hash →R (proven; empty files
  and symlinks never pair — zero bytes prove nothing about intent), else
  A+D. etc/home/fs-rw scopes have no baseline: present→A, whiteout→D,
  never M/R; hunks only where both sides' bytes exist. D without
  recoverable old bytes gets a header + note, never fabricated hunks.
- **Pending view.** `diff` shows what `apply` would still do: entries
  whose host state already equals the upper state are suppressed as
  already-applied (counted as `applied` in the summary, never silently
  dropped). Rerunning `diff` after a clean `apply` is therefore empty.
  `compare` stays host-independent (raw agent change sets).
- **Hunks come from a real hand-rolled diff (Hirschberg LCS, linear
  space).** Old bytes for project M/D:
  the host project file, accepted only when its sha256 equals the
  baseline hash (host untouched since the run is the dry-run norm);
  mismatch → the entry keeps its letter with an `host-changed` note and
  no hunks. A shows full-add hunks. `--stat` prints
  `path scope kind +adds -dels` + summary (unknown counts render `?`);
  `--patch=FILE` writes git-style `diff --git` output (`new file`,
  `deleted`, `rename from/to`, hunks). Bare `diff` == `--stat` to stderr.
  Exit 0; 69 when the run has no baseline/upper.
- **Non-isolate runs: honest mode.** No content was captured, so `diff`
  reports event-derived paths only (`created?`/`modified?`/`deleted?`/
  `renamed?` from `fs.open{create}`/write/`unlink`+`rmdir`/`rename`),
  headed by `content not captured (run with --isolate for content)`.
- **apply is validate-all-then-write (all-or-nothing).** Phase 1 loads
  the project change set and checks every entry against the host:
  M/D/R-old conflict iff host sha256 ≠ baseline sha (missing host on
  D = already-applied, ok); A conflicts iff host exists. Safety
  rejections (reported separately, same abort): absolute paths, `..`,
  empty, `.git/`-prefixed (repo metadata is reviewed, never written),
  symlink in any traversed *parent* component (`openat`+`O_NOFOLLOW` per
  component), non-regular upper entries (upper symlinks themselves are
  recreated as symlinks). A symlink in *final* position is validated per
  kind, never followed: A conflicts iff host exists; M-link proceeds iff
  the host still holds the baseline link (else conflict); M-file against
  a host symlink conflicts; D unlinks the link itself. Writes use
  temp+rename, so even a raced-in link is replaced, never traversed. Any conflict
  or rejection → print the report, write NOTHING, exit 65. Clean →
  write all, exit 0. `--dry-run` runs validation + report only (same
  exits). Without `--yes`/`--dry-run`: prompt on a tty, usage error
  (64) off-tty. R applies as A+D (same end state). etc/home/fs-rw
  scopes are never applied (AGENTS.md §3 "not for apply").
- **compare joins two runs' change sets** by (scope, path): A-only,
  B-only, both-same (kind + content hash equal; D equality needs equal
  baseline hashes), both-differ. Default human table to stderr;
  `--json` machine array to stdout. Either run lacking
  baseline/upper → 69.

## Consequences

- New exit 65 (apply aborted: conflicts and/or rejected paths, zero
  writes) joins the §2 rules; `diff`/`compare` stay 0/64/69.
- `diff --patch` output is informational (review aid); `apply` is
  authoritative (reads the upper, not the patch).
- Rename detection is content-hash exact: renames with any byte change
  report as A+D (documented in limitations, not a silent guess).
- `baseline.json` cost is one hashed walk at run start (STATUS notes
  the measured time on this repo).

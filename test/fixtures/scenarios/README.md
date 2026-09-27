# Golden scenario fixtures

Each `<name>/` holds `run.sh` (deterministic, network-free workload) and
`expected.jsonl` (normalised events, see `test/normalize.py`).

## Conventions (keep them — goldens depend on each)

- `run.sh` starts with `export PATH=/usr/bin:/bin` (fixed execvp search).
- No interpreted languages in scenarios: python startup file sets differ per
  release (import closure, .so link order) and cannot be normalised — threads
  and unix_sockets use compiled C helpers (`sg_threads.c`, `sg_sockets.c`,
  built by CMake into `test/helpers/`), exec_chain is pure sh+env. Helpers
  are found via `$SG_HELPERS` (set by the driver; env is never recorded) and
  normalise to `$HELPERS`.
- Work dir is `mktemp -d` (`/tmp/tmp.XXXXXXXXXX` → normalised to `$TMP`).
- Repo paths normalise to `$REPO`; `python3.V` / `cpython-3V` cover 3.10/3.12.
- Scripts exit 0 (handle expected failures internally with `|| true`).
- Threads must order milestones with pipes/joins; the tracer records stops in
  kernel order, so unsynchronised threads flake. `stat` polling is invisible
  (stat is never decoded) and may order creations.

## Regenerating goldens

```
SNOWGLOBE_UPDATE_GOLDENS=1 ctest -R golden --output-on-failure
```

(or `test/golden.py <snowglobe> <scenario-dir> <repo> --update` directly).
Regeneration additionally requires a CHANGELOG line describing why the
golden changed. Never regenerate to silence a red test you don't understand.

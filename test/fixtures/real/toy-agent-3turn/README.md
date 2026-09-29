# toy-agent-3turn — FALLBACK fixture (no real-agent recording)

This is **not a real Claude Code trace**. No `ANTHROPIC_API_KEY` and no
`claude` binary existed on the recording box, so per the Block 3 spec the
toy agent stands in as the 1C viewer's dev data, explicitly labelled. The
open issue "record real-agent fixtures" (see `../real/README.md`) stays
open — replace this directory with a real recording when keys are
available; do not delete it silently (1C may already reference it).

What it is: `examples/toy-agent/agent.py` (run_command → write_file +
http_get → done) run under `snowglobe run --upstream=openai=<mock>`
against `test/mockllm/server.py`. 125 normalised events, 3
`llm.request`/`llm.response` pairs, 9 blobs.

Layout: `events.jsonl` (normalised with `test/normalize.py`: P/T ids,
`$TMP`, `$REPO`, `127.0.0.1:PORT`) + `llm/` blobs. Blob request bodies
had the same three substitutions applied as strings, so the fixture is
free of machine specifics; blob network locations are concrete by design.

Regenerate (as uid 1000 on Linux):
```
D=$(mktemp -d) # /tmp/tmp.XXXXXXXXXX
# ... start mock with the toy 3-turn scenario, then:
OPENAI_API_KEY=sk-test-fixture-001 snowglobe run \
  --upstream=openai=http://127.0.0.1:$PORT --out=$D/run.sgr -- \
  python3 examples/toy-agent/agent.py --workdir $D/work \
  --data-url http://127.0.0.1:$PORT/test-data
grep -r sk-test-fixture-001 $D/run.sgr | wc -l  # must be 0
python3 test/normalize.py $D/run.sgr/events.jsonl --repo $PWD \
  > test/fixtures/real/toy-agent-3turn/events.jsonl
# copy llm/*, substitute $D→$TMP and 127.0.0.1:<port>→127.0.0.1:PORT
```

Key-safety: recorded with a throwaway `sk-test-*` key; `Authorization`
stored `REDACTED`; grep for the key over this directory is empty
(re-run that check after any regeneration).

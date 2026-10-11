# Demo GIF script (60 seconds)

Records a toy agent under `--isolate`, views the trace, diffs the
sandbox upper, and replays offline — every command below ran verbatim
(outputs are real). Needs a Linux box with userns + overlayfs
(`snowglobe doctor --isolate` shows both green — without them, stop
after the `view` step on a plain `run`).

Terminal: 80x24, recorder on the terminal only (`peek -d 1` or
`ffmpeg -f x11grab`). Keep one terminal for the mock, one for the
commands. Total wall time ≈ 60 s (the replay is `--realtime`; gaps here
are milliseconds — thinking-gap traces sleep minutes there, by design).

```sh
cd ~/src/snowglobe                         # repo root
cmake --preset debug && cmake --build --preset debug   # one-time build
mkdir -p /tmp/demo-w
cat > /tmp/demo-scn.json <<'EOF'
{"responses": [
 {"status": 200, "stream": false, "body": {"id": "chatcmpl-1", "model": "mock-model-1",
  "choices": [{"message": {"role": "assistant", "content": null,
   "tool_calls": [{"id": "call_1", "type": "function",
    "function": {"name": "run_command", "arguments": "{\"command\": \"echo tool-output-123\"}"}}]}}]}},
 {"status": 200, "stream": false, "body": {"id": "chatcmpl-2", "model": "mock-model-1",
  "choices": [{"message": {"role": "assistant", "content": null,
   "tool_calls": [{"id": "call_2", "type": "function",
    "function": {"name": "write_file", "arguments": "{\"path\": \"/tmp/demo-w/note.txt\", \"content\": \"hello snowglobe\"}"}},
    {"id": "call_3", "type": "function",
    "function": {"name": "http_get", "arguments": "{\"url\": \"http://127.0.0.1:18080/test-data\"}"}}]}}]}},
 {"status": 200, "stream": false, "body": {"id": "chatcmpl-3", "model": "mock-model-1",
  "choices": [{"message": {"role": "assistant", "content": "done"}}]}}]}
EOF
python3 test/mockllm/server.py --port 18080 --scenario /tmp/demo-scn.json &
```

Record (mock keeps running for the whole GIF — the agent's `http_get`
hits it directly; non-LLM traffic is honestly not stubbed):

```sh
OPENAI_API_KEY=demo PYTHONDONTWRITEBYTECODE=1 \
./build/debug/core/snowglobe run --isolate --project=/tmp/demo-w \
  --allow-env=OPENAI_API_KEY \
  --upstream=openai=http://127.0.0.1:18080 \
  --out /tmp/demo-llm.sgr -- \
  python3 examples/toy-agent/agent.py --workdir /tmp/demo-w \
  --data-url http://127.0.0.1:18080/test-data
# exit: 0 | events: 126 | 3 LLM turns
```

View, diff, replay (mock still up):

```sh
./build/debug/core/snowglobe view /tmp/demo-llm.sgr
# serving ... on http://127.0.0.1:7777   (open it: 3 turns on the Turns tab)
./build/debug/core/snowglobe diff /tmp/demo-llm.sgr
# A project note.txt +1 -0
./build/debug/core/snowglobe replay /tmp/demo-llm.sgr --realtime --out /tmp/demo-replay.sgr
# offline: 3 responses served, 0 unrecorded
# verdict: clean
```

Then `fg` + Ctrl-C the mock. The GIF ends on the viewer Turns tab
with the replay epilogue visible in the terminal.

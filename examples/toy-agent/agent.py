#!/usr/bin/env python3
"""Toy agent: OpenAI-format tool-calling loop (stdlib only, ~120 lines).

Tools: write_file {path, content}, run_command {command} (via sh -c),
http_get {url} (truncated to 2000 chars). Reads OPENAI_BASE_URL (the
Snowglobe proxy when run under `snowglobe run`) and OPENAI_API_KEY.
Deterministic: fixed tool spec, no timestamps/randomness; the mock drives
the whole dialogue from its scenario script. Demo agent for 1C/1D too —
keep it readable.

Usage: agent.py --workdir DIR --data-url URL [--model NAME] [--max-turns N]
Exit 0 prints the model's final content; exit 1 when the model never stops
calling tools (a real failure, never silent).
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.request

TOOLS = [{"type": "function", "function": {
    "name": "write_file",
    "description": "Write content to path",
    "parameters": {"type": "object", "properties": {
        "path": {"type": "string"}, "content": {"type": "string"}},
        "required": ["path", "content"]}}},
    {"type": "function", "function": {
    "name": "run_command",
    "description": "Run a shell command, capture output",
    "parameters": {"type": "object", "properties": {
        "command": {"type": "string"}}, "required": ["command"]}}},
    {"type": "function", "function": {
    "name": "http_get",
    "description": "GET a URL, return up to 2000 chars",
    "parameters": {"type": "object", "properties": {
        "url": {"type": "string"}}, "required": ["url"]}}}]


def chat(base, key, model, messages):
    req = {"model": model, "messages": messages, "tools": TOOLS}
    data = json.dumps(req).encode()
    # SDK-shaped path: the injected OPENAI_BASE_URL already ends in /v1.
    r = urllib.request.Request(base.rstrip("/") + "/chat/completions",
                               data=data, method="POST",
                               headers={"Content-Type": "application/json",
                                        "Authorization": "Bearer " + key})
    with urllib.request.urlopen(r, timeout=120) as resp:
        return json.loads(resp.read().decode())


def run_tool(tc):
    fn = tc["function"]
    name, args = fn["name"], json.loads(fn.get("arguments") or "{}")
    if name == "write_file":
        with open(args["path"], "w") as f:
            f.write(args["content"])
        return "wrote %d bytes to %s" % (len(args["content"]), args["path"])
    if name == "run_command":
        p = subprocess.run(["sh", "-c", args["command"]], capture_output=True,
                           text=True, timeout=60)
        return "$ %s\nrc=%d\n%s%s" % (args["command"], p.returncode,
                                      p.stdout, p.stderr)
    if name == "http_get":
        with urllib.request.urlopen(args["url"], timeout=60) as resp:
            return resp.read().decode()[:2000]
    return "unknown tool: " + name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--data-url", required=True)
    ap.add_argument("--model", default="mock-model-1")
    ap.add_argument("--max-turns", type=int, default=5)
    a = ap.parse_args()
    base = os.environ.get("OPENAI_BASE_URL")
    if not base:
        print("agent: OPENAI_BASE_URL is not set", file=sys.stderr)
        return 2
    key = os.environ.get("OPENAI_API_KEY", "test-key")
    _ = (a.workdir, a.data_url)  # carried inside tool args, not globals
    messages = [{"role": "user",
                 "content": "Do the three tasks: save a note, run a command, "
                            "fetch the data URL. Reply done when finished."}]
    for _ in range(a.max_turns):
        msg = chat(base, key, a.model, messages)["choices"][0]["message"]
        messages.append({"role": "assistant", "content": msg.get("content"),
                         "tool_calls": msg.get("tool_calls")})
        calls = msg.get("tool_calls") or []
        if not calls:
            print(msg.get("content") or "")
            return 0
        for tc in calls:
            out = run_tool(tc)
            messages.append({"role": "tool", "tool_call_id": tc["id"],
                             "content": out})
    print("agent: model never finished", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

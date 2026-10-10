#!/usr/bin/env python3
"""Shared harness for snowglobe replay integration tests (stdlib only).

Every test records a run under `snowglobe run` (live mock upstream),
then re-executes it with `snowglobe replay` fully offline (no mock on
the LLM path, API keys scrubbed) and asserts on the exit code plus
<new-run>/replay-report.json.

Path discipline: working dirs live under mkdtemp(prefix="tmp.") so the
/tmp/tmp.XXXXXXXX prefix matches test/normalize.py's TMP_RE on both the
C++ compare side and any string substitution here. Ports are ephemeral
per run (the comparer folds 127.0.0.1:N to PORT).
"""
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
import time

UTIL_DIR = os.path.dirname(os.path.abspath(__file__))
PROXY_DIR = os.path.join(os.path.dirname(UTIL_DIR), "proxy")


def skip(reason):
    print("SKIP: " + reason)
    sys.exit(0)


def check_linux(snowglobe):
    if platform.system() != "Linux":
        skip("requires Linux ptrace backend")
    if not os.path.exists(snowglobe):
        skip("binary not found: " + snowglobe)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_port(port, timeout=10.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


class Mock:
    """Mock LLM instance (same contract as proxy tests' Mock)."""

    def __init__(self, server_py, port, scenario=None):
        self.work = tempfile.mkdtemp(prefix="tmp.")
        self.sent = os.path.join(self.work, "sent")
        self.hdrs = os.path.join(self.work, "hdrs.jsonl")
        cmd = [sys.executable, server_py, "--port", str(port),
               "--headers-log", self.hdrs, "--sent-dir", self.sent]
        if scenario is not None:
            scn = os.path.join(self.work, "scn.json")
            with open(scn, "w") as f:
                json.dump(scenario, f)
            cmd += ["--scenario", scn]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
        self.port = port
        self.base = "http://127.0.0.1:%d" % port
        if not wait_port(port):
            out = self.proc.stdout.read() if self.proc.stdout else ""
            self.proc.kill()
            raise RuntimeError("mock never listened: " + out)

    def posts(self):
        recs = []
        if os.path.exists(self.hdrs):
            with open(self.hdrs) as f:
                for line in f:
                    line = line.strip()
                    if line:
                        recs.append(json.loads(line))
        return recs

    def stop(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        if self.proc.stdout:
            self.proc.stdout.close()

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.work, ignore_errors=True)


def tc(i, name, args):
    return {"id": "call_%d" % i, "type": "function",
            "function": {"name": name, "arguments": json.dumps(args)}}


def toy_scenario(wd, durl):
    return {"responses": [
        {"status": 200, "stream": False, "body": {
            "id": "chatcmpl-1", "model": "mock-model-1",
            "choices": [{"message": {"role": "assistant", "content": None,
                                     "tool_calls": [tc(
                                         1, "run_command",
                                         {"command": "echo tool-output-123"})]}}]}},
        {"status": 200, "stream": False, "body": {
            "id": "chatcmpl-2", "model": "mock-model-1",
            "choices": [{"message": {"role": "assistant", "content": None,
                                     "tool_calls": [
                                         tc(2, "write_file",
                                            {"path": wd + "/note.txt",
                                             "content": "hello snowglobe"}),
                                         tc(3, "http_get", {"url": durl})]}}]}},
        {"status": 200, "stream": False, "body": {
            "id": "chatcmpl-3", "model": "mock-model-1", "choices": [
                {"message": {"role": "assistant",
                             "content": "done"}}]}},
    ]}


def run_sg(snowglobe, sg_args, out_dir, env_extra=None, scrub=(), timeout=180):
    env = dict(os.environ)
    env.update(env_extra or {})
    # No .pyc writes next to traced scripts: bytecode paths differ per run
    # (repo vs tmp workdir) and would pollute the fs compare with noise.
    env.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    for k in scrub:
        env.pop(k, None)
    return subprocess.run([snowglobe, "run", "--out=" + out_dir, *sg_args],
                          capture_output=True, text=True, env=env,
                          timeout=timeout)


def run_replay(snowglobe, orig, replay_args, out_dir, env_extra=None,
               scrub=(), timeout=180):
    env = dict(os.environ)
    env.update(env_extra or {})
    env.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    for k in scrub:
        env.pop(k, None)
    return subprocess.run(
        [snowglobe, "replay", orig, "--out=" + out_dir, *replay_args],
        capture_output=True, text=True, env=env, timeout=timeout)


def load_events(out_dir):
    evs = []
    with open(os.path.join(out_dir, "events.jsonl")) as f:
        for line in f:
            line = line.strip()
            if line:
                evs.append(json.loads(line))
    return evs


def load_report(new_dir):
    with open(os.path.join(new_dir, "replay-report.json")) as f:
        return json.load(f)


def sha_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def res_blob_shas(run_dir):
    """Map recorded-id -> sha256 of the response blob (via llm.response)."""
    out = {}
    for e in load_events(run_dir):
        if e.get("ev") == "llm.response":
            out[e["id"]] = sha_file(os.path.join(run_dir, e["res"]))
    return out

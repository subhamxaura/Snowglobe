#!/usr/bin/env python3
"""Shared harness for snowglobe proxy integration tests (stdlib only).

Conventions: every test is `test.py <snowglobe-bin> <mockllm-dir>
<toyagent-dir>`, runs on Linux only (skips otherwise), starts its own mock
on a fixed localhost port, drives a traced client through `snowglobe run
--upstream=openai|anthropic=<mock>`, and cleans up (mock killed, temp dirs
removed) even on failure. Helper clients in this directory run UNDER
snowglobe (they need the injected *BASE_URL env); tests run outside.
"""
import hashlib
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

PROXY_DIR = os.path.dirname(os.path.abspath(__file__))


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
    """A mock LLM instance. `scenario` is a dict (written to scn.json)."""

    def __init__(self, server_py, port, scenario=None, extra=()):
        self.work = tempfile.mkdtemp(prefix="sg-mock-")
        self.sent = os.path.join(self.work, "sent")
        self.hdrs = os.path.join(self.work, "hdrs.jsonl")
        cmd = [sys.executable, server_py, "--port", str(port),
               "--headers-log", self.hdrs, "--sent-dir", self.sent,
               *extra]
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

    def header_records(self):
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


def run_sg(snowglobe, sg_args, out_dir, env_extra=None, timeout=120):
    """Run `snowglobe run --out=out_dir <sg_args>`. Returns CompletedProcess
    (stdout/stderr captured; the child's stdout passes through)."""
    env = dict(os.environ)
    env.update(env_extra or {})
    return subprocess.run([snowglobe, "run", "--out=" + out_dir, *sg_args],
                          capture_output=True, text=True, env=env,
                          timeout=timeout)


def load_events(out_dir):
    evs = []
    with open(os.path.join(out_dir, "events.jsonl")) as f:
        for line in f:
            line = line.strip()
            if line:
                evs.append(json.loads(line))
    return evs


def llm_pairs(evs):
    reqs = {e["id"]: e for e in evs if e.get("ev") == "llm.request"}
    resps = {e["id"]: e for e in evs if e.get("ev") == "llm.response"}
    return reqs, resps


def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def grep_bytes(root, needle):
    """Return list of files under root containing needle (bytes)."""
    hits = []
    nb = needle.encode() if isinstance(needle, str) else needle
    for dirpath, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            try:
                with open(p, "rb") as f:
                    if nb in f.read():
                        hits.append(p)
            except OSError:
                pass
    return hits


def direct_post(mock_base, path, body, headers=None, timeout=120):
    """POST outside the proxy (baseline / mock self-check)."""
    data = body if isinstance(body, bytes) else json.dumps(body).encode()
    r = urllib.request.Request(mock_base + path, data=data, method="POST",
                               headers={"Content-Type": "application/json",
                                        **(headers or {})})
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()

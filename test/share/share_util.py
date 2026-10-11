#!/usr/bin/env python3
"""Shared harness for snowglobe share/verify integration tests.

Reuses the proxy suite's Mock/runner (same contract) and adds a minimal
`view` starter plus sha256 helpers. Stdlib only.
"""
import hashlib
import json
import os
import platform
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

UTIL_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(UTIL_DIR), "proxy"))
from sgtest_util import (check_linux, free_port, Mock, run_sg,  # noqa: E402
                         load_events)


def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def tc(i, name, args):
    return {"id": "call_%d" % i, "type": "function",
            "function": {"name": name, "arguments": json.dumps(args)}}


def two_turn_scenario(wd):
    return {"responses": [
        {"status": 200, "stream": False, "body": {
            "id": "chatcmpl-1", "model": "mock-model-1",
            "choices": [{"message": {"role": "assistant", "content": None,
                                     "tool_calls": [tc(
                                         1, "write_file",
                                         {"path": wd + "/note.txt",
                                          "content": "share me"})]}}]}},
        {"status": 200, "stream": False, "body": {
            "id": "chatcmpl-2", "model": "mock-model-1", "choices": [
                {"message": {"role": "assistant",
                             "content": "done"}}]}},
    ]}


def run_cli(args, timeout=120):
    return subprocess.run(args, capture_output=True, text=True,
                          timeout=timeout)


class View:
    """A `snowglobe view` server on an ephemeral port (stdlib only)."""

    def __init__(self, snowglobe, target):
        self.proc = subprocess.Popen(
            [snowglobe, "view", target, "--port=0"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.url = self._wait_url()

    def _wait_url(self):
        t0 = time.monotonic()
        err = b""
        while time.monotonic() - t0 < 15:
            if self.proc.poll() is not None:
                rest = self.proc.stderr.read() or b""
                raise RuntimeError("view exited early: " +
                                   (err + rest).decode(errors="replace"))
            r, _, _ = select.select([self.proc.stderr], [], [], 0.2)
            if r:
                chunk = os.read(self.proc.stderr.fileno(), 4096)
                if not chunk:
                    continue
                err += chunk
                m = re.search(rb"http://127\.0\.0\.1:\d+", err)
                if m:
                    url = m.group(0).decode()
                    for _ in range(100):
                        try:
                            with urllib.request.urlopen(
                                    url + "/api/manifest",
                                    timeout=5) as resp:
                                if resp.status == 200:
                                    return url
                        except OSError:
                            time.sleep(0.1)
                    raise RuntimeError("view never ready")
        raise RuntimeError("view never printed a URL")

    def get(self, path):
        with urllib.request.urlopen(self.url + path,
                                    timeout=10) as resp:
            return resp.status, resp.read()

    def stop(self):
        try:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(timeout=10)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass

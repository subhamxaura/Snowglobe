#!/usr/bin/env python3
"""replay_toolchange (mutation a): non-LLM tool output differs on replay.

The toy's http_get fetches --data-url directly (non-LLM traffic is NOT
stubbed — the honest wall). Recording serves mock /test-data; replay
serves MUTATED bytes from a tiny inline server. The agent therefore sends
a different turn-2 request body: the recorded response still serves (same
endpoint, in order — never wrong-order), the agent finishes, but the
report names turn 2 as diverged and replay exits 65.
"""
import json
import os
import shutil
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from replay_util import (check_linux, free_port, Mock, toy_scenario,
                         run_sg, run_replay, load_report)

SNOWGLOBE, MOCKLLM, TOYAGENT = sys.argv[1], sys.argv[2], sys.argv[3]
AGENT = os.path.join(TOYAGENT, "agent.py")
SCRUB = ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN",
         "GEMINI_API_KEY", "GOOGLE_API_KEY")


class Mutated(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/test-data":
            data = json.dumps({"msg": "MUTATED-second-run", "n": 999}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_response(404)
            self.end_headers()


def main():
    check_linux(SNOWGLOBE)
    # Same-root discipline as replay_clean, except the replay data server
    # is a MUTATED one on the SAME port (the recorded URL is frozen).
    work = tempfile.mkdtemp(prefix="tmp.")
    work2 = tempfile.mkdtemp(prefix="tmp.")
    mock = None
    srv = None
    try:
        port = free_port()
        wd = os.path.join(work, "w")
        os.makedirs(wd)
        durl = "http://127.0.0.1:%d/test-data" % port
        mock = Mock(os.path.join(MOCKLLM, "server.py"), port,
                    toy_scenario(wd, durl))
        orig = os.path.join(work, "orig.sgr")
        r = run_sg(SNOWGLOBE,
                   ["--upstream=openai=" + mock.base, "--",
                    sys.executable, AGENT, "--workdir", wd,
                    "--data-url", durl],
                   orig, env_extra={"OPENAI_API_KEY": "sk-test-mut-a"})
        if r.returncode != 0:
            print("FAIL: record exited %d\n%s%s"
                  % (r.returncode, r.stdout, r.stderr))
            return 1
        mock.cleanup()
        mock = None
        # Replay against MUTATED data on the SAME port (fresh workdir).
        ThreadingHTTPServer.allow_reuse_address = True
        srv = ThreadingHTTPServer(("127.0.0.1", port), Mutated)
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        wd2 = os.path.join(work2, "w")
        os.makedirs(wd2)
        new = os.path.join(work, "new.sgr")
        r2 = run_replay(
            SNOWGLOBE, orig,
            ["--", sys.executable, AGENT, "--workdir", wd2,
             "--data-url", durl],
            new, scrub=SCRUB)
        if r2.returncode != 65:
            print("FAIL: replay exited %d, want 65 (diverged)\n%s%s"
                  % (r2.returncode, r2.stdout, r2.stderr))
            return 1
        rep = load_report(new)
        llm = rep["categories"]["llm"]
        if llm["status"] != "diverged":
            print("FAIL: llm not diverged: %s" % llm)
            return 1
        if not any("turn 2" in e for e in llm["replay_only"]):
            print("FAIL: divergence does not name turn 2: %s"
                  % llm["replay_only"])
            return 1
        # Side effects still match (same files/processes/net shape).
        for cat in ("fs", "proc", "net", "exit"):
            if rep["categories"][cat]["status"] != "identical":
                print("FAIL: side category %s should stay identical: %s"
                      % (cat, rep["categories"][cat]))
                return 1
        print("PASS replay_toolchange: turn-2 divergence named, exit 65")
        return 0
    finally:
        if mock is not None:
            mock.cleanup()
        if srv is not None:
            srv.shutdown()
            srv.server_close()
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(work2, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

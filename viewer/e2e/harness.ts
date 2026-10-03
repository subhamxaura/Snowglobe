// Shared e2e harness: stage a servable run dir, spawn `snowglobe view`,
// track every request (non-localhost fails the test).
import { execFileSync, spawn, type ChildProcess } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import type { Page } from "@playwright/test";

export const BIN = process.env.SNOWGLOBE_BIN ?? "";
export const FIXTURE = process.env.FIXTURE_DIR ?? "";

export function stageFixture(): string {
  const dir = join(
    tmpdir(),
    `sg-view-${process.pid}-${Math.floor(Math.random() * 1e6)}`,
  );
  mkdirSync(dir, { recursive: true });
  cpSync(FIXTURE, dir, { recursive: true });
  const lines = readFileSync(join(dir, "events.jsonl"), "utf8")
    .split("\n")
    .filter((l) => l.trim()).length;
  writeFileSync(
    join(dir, "manifest.json"),
    JSON.stringify({
      schema: 0,
      snowglobe_version: "e2e",
      cmd: ["e2e"],
      cwd: "/tmp",
      event_count: lines,
      last_hash: "0",
    }),
  );
  return dir;
}

export interface Served {
  proc: ChildProcess;
  url: string;
  external: string[];
  stop: () => Promise<void>;
}

export async function serve(dir: string): Promise<Served> {
  const proc = spawn(BIN, ["view", dir, "--port=0"], { stdio: ["ignore", "ignore", "pipe"] });
  let err = "";
  const url = await new Promise<string>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("view never printed a URL: " + err)), 10000);
    proc.stderr?.on("data", (d: Buffer) => {
      err += d.toString();
      const m = err.match(/http:\/\/127\.0\.0\.1:\d+/);
      if (m) {
        clearTimeout(timer);
        resolve(m[0]);
      }
    });
    proc.on("exit", (c) => reject(new Error(`view exited ${c}: ${err}`)));
  });
  // Readiness: poll /api/manifest (server binds before first poll).
  for (let i = 0; i < 100; i++) {
    try {
      execFileSync("curl", ["-sf", `${url}/api/manifest`], { timeout: 2000 });
      break;
    } catch {
      await sleep(100);
    }
    if (i === 99) throw new Error("view server never became ready");
  }
  return {
    proc,
    url,
    external: [],
    stop: async () => {
      proc.kill("SIGTERM");
      await sleep(300);
    },
  };
}

export function watchExternal(page: Page, external: string[]): void {
  page.on("request", (req) => {
    const host = new URL(req.url()).hostname;
    if (host !== "127.0.0.1" && host !== "localhost") {
      external.push(req.url());
    }
  });
}

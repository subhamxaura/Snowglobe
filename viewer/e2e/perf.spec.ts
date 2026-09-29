// Perf: a 50 MB trace opens in < 3 s (streaming parse + virtualized
// render), and scrolling stays windowed (bounded DOM, correct rows).
import { execFileSync } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { expect, test } from "@playwright/test";
import { serve, watchExternal } from "./harness";

const PY = process.env.PYTHON ?? "python3";

function bigTrace(): string {
  const dir = join(tmpdir(), `sg-big-${process.pid}`);
  mkdirSync(dir, { recursive: true });
  execFileSync(PY, [join(__dirname, "gen-big-trace.py"), dir, "50"], {
    timeout: 120000,
  });
  writeFileSync(
    join(dir, "manifest.json"),
    JSON.stringify({ schema: 0, event_count: -1, cmd: ["big"], last_hash: "0" }),
  );
  return dir;
}

test("50 MB trace opens in under 3 s", async ({ page }) => {
  const dir = bigTrace();
  const srv = await serve(dir);
  const external: string[] = [];
  watchExternal(page, external);
  try {
    const t0 = Date.now();
    await page.goto(srv.url, { waitUntil: "load" });
    await page.getByRole("tab", { name: "timeline" }).click();
    await expect(page.getByTestId("timeline-count")).toContainText("events", {
      timeout: 15000,
    });
    // All events parsed: the count line shows the parsed total, not a prefix.
    const text = (await page.getByTestId("timeline-count").textContent()) ?? "";
    const m = text.match(/([\d,]+) \/ ([\d,]+) events/);
    expect(m).not.toBeNull();
    expect(m![1]).toBe(m![2]);
    const dt = Date.now() - t0;
    expect(dt).toBeLessThan(3000);
    // Scroll stays windowed: bounded DOM rows near the bottom.
    const list = page.getByTestId("vlist");
    await list.evaluate((el) => el.scrollTo(0, el.scrollHeight));
    await page.waitForTimeout(300);
    const rows = await list.locator("[data-seq]").count();
    expect(rows).toBeLessThan(120);
    expect(rows).toBeGreaterThan(0);
    expect(external).toEqual([]);
  } finally {
    await srv.stop();
  }
});

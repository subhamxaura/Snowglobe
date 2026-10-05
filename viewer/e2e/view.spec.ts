// Acceptance: the toy-agent fixture renders 3 turns, the turn-2
// write_file links its fs.open, and nothing leaves localhost.
// Sidecar path (links.json present) and heuristic fallback (absent)
// are separate tests — one assertion must cover each.
import { expect, test } from "@playwright/test";
import { linkRun, maybeScreenshot, serve, stageFixture, watchExternal } from "./harness";

test("toy-agent fixture: 3 turns, linked file write, offline", async ({
  page,
}) => {
  const dir = stageFixture();
  linkRun(dir); // sidecar present: exact membership + basis badges
  const srv = await serve(dir);
  const external: string[] = [];
  watchExternal(page, external);
  try {
    await page.goto(srv.url, { waitUntil: "load" });
    await expect(page.getByTestId("manifest")).toContainText("125 events", {
      timeout: 10000,
    });
    // Turns tab is default: exactly 3 turns.
    await expect(page.getByTestId("turn-0")).toBeVisible({ timeout: 10000 });
    await expect(page.getByTestId("turn-1")).toBeVisible();
    await expect(page.getByTestId("turn-2")).toBeVisible();
    // Sidecar truth: membership comes from links.json, labelled.
    await expect(page.getByTestId("link-source")).toContainText("sidecar");
    // Turn 1 (id 0) ran run_command: the sh -c exec carries argv-match.
    await expect(page.getByTestId("basis-0-argv-match")).toBeVisible();
    // Turn 2 (id 1) ran write_file + http_get; the write_file path links
    // the fs.open(write) side effect under exactly that turn.
    await expect(page.getByTestId("tool-1-0")).toContainText("write_file");
    await expect(page.getByTestId("linked-1")).toContainText("note.txt");
    await expect(page.getByTestId("linked-1")).toContainText("[window]");
    await expect(page.getByTestId("turn-text-2")).toContainText("done");
    await maybeScreenshot(page, "turns-toy-agent.png");
    // Other views render on the same data.
    await page.getByRole("tab", { name: "timeline" }).click();
    await expect(page.getByTestId("timeline-count")).toContainText("125 / 125 events");
    await page.getByRole("tab", { name: "files" }).click();
    await expect(page.getByTestId("files")).toContainText("note.txt");
    await page.getByRole("tab", { name: "network" }).click();
    await expect(page.getByTestId("network")).toContainText("llm.request");
    expect(external).toEqual([]);
  } finally {
    await srv.stop();
  }
});

test("toy-agent fixture without links.json: heuristic fallback is labelled", async ({
  page,
}) => {
  const dir = stageFixture(); // no linkRun: /api/links 404s
  const srv = await serve(dir);
  try {
    await page.goto(srv.url, { waitUntil: "load" });
    await expect(page.getByTestId("turn-1")).toBeVisible({ timeout: 10000 });
    // Fallback path: the old path heuristic applies and says so.
    await expect(page.getByTestId("link-source")).toContainText("heuristic");
    await expect(page.getByTestId("linked-1")).toContainText("note.txt");
  } finally {
    await srv.stop();
  }
});

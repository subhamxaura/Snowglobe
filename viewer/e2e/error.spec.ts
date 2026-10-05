// Block 3 real-fixture smoke: the error recording (claude-code-1-error)
// must render — status, raw body toggle, no blank screen — offline.
// The success flow stays on toy-agent (view.spec.ts) until issue #2's
// recording lands; screenshots refresh when either changes.
import { expect, test } from "@playwright/test";
import {
  ERROR_FIXTURE,
  maybeScreenshot,
  serve,
  stageFixtureDir,
  watchExternal,
} from "./harness";

test("claude-code-1-error: error turns render status, envelope, raw body", async ({
  page,
}) => {
  test.skip(!ERROR_FIXTURE, "ERROR_FIXTURE_DIR not set");
  const dir = stageFixtureDir(ERROR_FIXTURE);
  const srv = await serve(dir);
  const external: string[] = [];
  watchExternal(page, external);
  try {
    await page.goto(srv.url, { waitUntil: "load" });
    await expect(page.getByTestId("manifest")).toContainText("735 events", {
      timeout: 10000,
    });
    // No blank screen: the turns view renders with the first error turn.
    await expect(page.getByTestId("turns")).toBeVisible({ timeout: 10000 });
    // The HEAD probe is llm id 0 — it must never appear as a turn.
    await expect(page.getByTestId("turn-0")).toHaveCount(0);
    // Real status path: HTTP 401 + parsed Anthropic envelope on turn id 1.
    await expect(page.getByTestId("turn-1")).toBeVisible();
    await expect(page.getByTestId("turn-1")).toContainText("status 401");
    await expect(page.getByTestId("turn-error-1")).toContainText("HTTP 401");
    await expect(page.getByTestId("turn-error-1")).toContainText(
      "authentication_error",
    );
    await expect(page.getByTestId("turn-error-1")).toContainText(
      "API key is invalid.",
    );
    // 11 error turns: ids 1..11, nothing after.
    await expect(page.getByTestId("turn-11")).toBeVisible();
    await expect(page.getByTestId("turn-12")).toHaveCount(0);
    // Staged without links.json: the heuristic fallback applies and says so.
    await expect(page.getByTestId("link-source")).toContainText("heuristic");
    // Raw body toggle opens on the real envelope JSON.
    await page.getByTestId("raw-body-1").locator("summary").click();
    await expect(page.getByTestId("raw-body-1")).toContainText('"type":"error"');
    // No load error / blank screen.
    await expect(page.getByTestId("error")).toHaveCount(0);
    await maybeScreenshot(page, "turns-error-401.png");
    // Other views render on the same data.
    await page.getByRole("tab", { name: "timeline" }).click();
    await expect(page.getByTestId("timeline-count")).toContainText(
      "735 / 735 events",
    );
    expect(external).toEqual([]);
  } finally {
    await srv.stop();
  }
});

import { defineConfig, devices } from "@playwright/test";

// Serves nothing itself: each test spawns `snowglobe view <fixture>`
// (SNOWGLOBE_BIN env) and drives the embedded page. No external network:
// tests fail on any non-localhost request.
export default defineConfig({
  testDir: "./e2e",
  timeout: 60000,
  fullyParallel: false,
  retries: 0,
  reporter: "line",
  use: {
    ...devices["Desktop Chrome"],
    baseURL: "http://127.0.0.1",
  },
});

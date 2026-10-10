// replayBadge (ADR-0010): one-line header badge for replay runs.
// Pure function of manifest + report (network-free, fixture-free).
import { describe, expect, it } from "vitest";
import { replayBadge } from "../lib/load";
import type { Manifest, ReplayReport } from "../lib/types";

const M = (o: Record<string, unknown>): Manifest =>
  ({
    schema: 0,
    snowglobe_version: "v",
    started: "",
    finished: null,
    cmd: [],
    cwd: "",
    event_count: 0,
    last_hash: "",
    ...o,
  }) as unknown as Manifest;

const R = (statuses: Record<string, string>, unrecorded = 0): ReplayReport =>
  ({
    version: 1,
    original: "/o.sgr",
    replay: "/r.sgr",
    original_exit: 0,
    replay_exit: 0,
    turns: { original: 3, replay: 3, match: true },
    order_matches: true,
    unrecorded,
    categories: Object.fromEntries(
      Object.entries(statuses).map(([k, s]) => [k, { status: s, detail: "" }]),
    ),
  }) as unknown as ReplayReport;

describe("replayBadge", () => {
  it("is null without a replay_of link", () => {
    expect(replayBadge(M({}), null)).toBeNull();
    expect(replayBadge(M({}), R({ llm: "identical" }))).toBeNull();
  });
  it("names the original without a report", () => {
    expect(replayBadge(M({ replay_of: "/x/orig.sgr" }), null)).toBe(
      "Replay of /x/orig.sgr",
    );
  });
  it("reports clean", () => {
    expect(
      replayBadge(
        M({ replay_of: "/x/orig.sgr" }),
        R({ llm: "identical", fs: "identical" }),
      ),
    ).toBe("Replay of /x/orig.sgr · verdict: clean");
  });
  it("names diverged categories + unrecorded count", () => {
    expect(
      replayBadge(M({ replay_of: "/x/orig.sgr" }), R({ llm: "unrecorded", fs: "diverged" }, 1)),
    ).toBe("Replay of /x/orig.sgr · verdict: DIVERGED (llm,fs) · 1 unrecorded");
  });
});

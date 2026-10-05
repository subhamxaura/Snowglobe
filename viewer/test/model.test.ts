// model.ts: schema-0 types + turn layer. Real fixture (toy-agent, 3 turns)
// plus synthetic error/probe/disconnect/structured-net shapes from the
// real Claude Code runs (error turns count, HEAD probes don't).
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import {
  TurnIndex,
  buildTurns,
  endpointOf,
  hostKey,
  isProbeRequest,
  isWriteOpen,
} from "../lib/model";
import type { LinksDoc } from "../lib/model";
import type { TraceEvent } from "../lib/types";

const FIX = join(__dirname, "..", "..", "test", "fixtures", "real", "toy-agent-3turn");
const evs = (): TraceEvent[] =>
  readFileSync(join(FIX, "events.jsonl"), "utf8")
    .split("\n")
    .filter((l) => l.trim())
    .map((l) => JSON.parse(l) as TraceEvent);

const E = (o: Record<string, unknown>): TraceEvent =>
  ({ t_ms: 0, ...o }) as unknown as TraceEvent;

describe("toy-agent fixture turns", () => {
  it("builds 3 turns, no probes, stable spans", () => {
    const turns = buildTurns(evs());
    expect(turns.length).toBe(3);
    expect(turns.every((t) => !t.error)).toBe(true);
    const idx = new TurnIndex(evs());
    // Spans are [reqSeq, nextReqSeq): last event belongs to turn 3.
    expect(idx.getTurnForEvent(1_000_000)?.id).toBe(2);
    expect(idx.getTurnForEvent(-1)).toBeNull();
  });
});

describe("error turns count, probes don't", () => {
  const synth: TraceEvent[] = [
    E({ ev: "llm.request", id: 0, provider: "anthropic", method: "POST", model: "m", bytes: 100, seq: 10 }),
    E({ ev: "llm.response", id: 0, status: 200, bytes: 50, seq: 11 }),
    E({ ev: "fs.open", path: "/tmp/note.txt", write: true, seq: 12 }),
    E({ ev: "llm.request", id: 1, provider: "unknown", method: "HEAD", model: null, bytes: 0, seq: 13 }),
    E({ ev: "llm.response", id: 1, status: 502, bytes: 0, seq: 14 }),
    E({ ev: "llm.request", id: 2, provider: "anthropic", method: "POST", model: "m", bytes: 90, seq: 15 }),
    E({ ev: "llm.response", id: 2, status: 401, bytes: 198, seq: 16 }),
  ];
  it("counts the 401 as an error turn, skips the HEAD 502", () => {
    const turns = buildTurns(synth);
    expect(turns.map((t) => t.id)).toEqual([0, 2]);
    expect(turns[1].error).toBe(true);
  });
  it("getTurnForEvent keeps a stable interface over probe seqs", () => {
    const idx = new TurnIndex(synth);
    expect(idx.getTurnForEvent(9)).toBeNull();
    expect(idx.getTurnForEvent(12)?.id).toBe(0); // side effect under turn 0
    expect(idx.getTurnForEvent(14)?.id).toBe(0); // probe lives in turn 0's span
    expect(idx.getTurnForEvent(16)?.id).toBe(2);
  });
  it("flags probes by HEAD or model-less+bodyless", () => {
    expect(isProbeRequest({ id: 1, provider: "u", method: "HEAD", path: "/", model: null, bytes: 0, stream: false, seq: 0 })).toBe(true);
    expect(isProbeRequest({ id: 1, provider: "u", method: "POST", path: "/", model: null, bytes: 0, stream: false, seq: 0 })).toBe(true);
    // Gemini-style: no model in JSON but a real body.
    expect(isProbeRequest({ id: 1, provider: "gemini", method: "POST", path: "/", model: null, bytes: 500, stream: false, seq: 0 })).toBe(false);
  });
});

describe("network endpoints", () => {
  it("handles disconnect + structured fields + legacy addr", () => {
    expect(endpointOf(E({ ev: "net.disconnect", ok: true }))).toEqual({
      family: "unspec",
      label: "(disconnect)",
      bucket: "other",
    });
    expect(endpointOf(E({ ev: "net.connect", family: "ipv4", ip: "1.2.3.4", port: 443 }))).toMatchObject({
      bucket: "tcp",
      label: "1.2.3.4:443",
    });
    expect(endpointOf(E({ ev: "net.connect", family: "unix", path: "/run/x.sock" }))).toMatchObject({
      bucket: "unix",
      label: "unix:/run/x.sock",
    });
    expect(endpointOf(E({ ev: "net.connect", addr: "127.0.0.1:PORT" }))).toMatchObject({ bucket: "tcp" });
    expect(hostKey(E({ ev: "proc.exec", path: "/bin/x" }))).toBeNull();
  });
});

describe("file helpers", () => {
  it("spots write opens", () => {
    expect(isWriteOpen(E({ ev: "fs.open", path: "/a", write: true }))).toBe(true);
    expect(isWriteOpen(E({ ev: "fs.open", path: "/a", write: false }))).toBe(false);
  });
});

describe("links.json sidecar (TurnIndex source)", () => {
  const synth: TraceEvent[] = [
    E({ ev: "llm.request", id: 0, provider: "m", method: "POST", model: "m", bytes: 10, seq: 10 }),
    E({ ev: "llm.response", id: 0, status: 200, bytes: 5, seq: 11 }),
    E({ ev: "fs.open", path: "/tmp/a", write: true, seq: 12 }),
    E({ ev: "fs.open", path: "/tmp/b", write: true, seq: 13 }),
    E({ ev: "llm.request", id: 1, provider: "m", method: "POST", model: "m", bytes: 9, seq: 14 }),
    E({ ev: "llm.response", id: 1, status: 200, bytes: 5, seq: 15 }),
  ];
  const links = (turns: LinksDoc["turns"]): LinksDoc => ({ version: 1, turns });
  it("reports heuristic source without links", () => {
    const idx = new TurnIndex(synth);
    expect(idx.source).toBe("heuristic");
    // span heuristic: everything from req 10 belongs to turn 0
    expect(idx.getTurnForEvent(12)?.id).toBe(0);
    expect(idx.linkOf(12)).toBeNull();
  });
  it("sidecar wins where it speaks, heuristic elsewhere", () => {
    const idx = new TurnIndex(
      synth,
      links([
        {
          turn: 1,
          llm: { req: 14, res: 15, tools: [] },
          attributed: [{ seq: 13, basis: "lineage", confidence: "high" }],
          unattributed: [{ seq: 12, reason: "pre-turn" }],
        },
      ]),
    );
    expect(idx.source).toBe("sidecar");
    // exact hit overrides the span (13 would heuristically be turn 0)
    expect(idx.getTurnForEvent(13)?.id).toBe(1);
    expect(idx.linkOf(13)).toEqual({
      turn: expect.objectContaining({ id: 1 }),
      basis: "lineage",
      confidence: "high",
    });
    // explicitly unattributed stays visible with its reason
    expect(idx.linkOf(12)).toEqual({ unattributed: "pre-turn" });
    // unlisted seqs keep the old span answer
    expect(idx.getTurnForEvent(11)?.id).toBe(0);
    expect(idx.linkOf(11)).toBeNull();
  });
  it("ignores non-v1 sidecars (forward compat falls back)", () => {
    const idx = new TurnIndex(synth, { version: 99, turns: [] });
    expect(idx.source).toBe("heuristic");
    expect(idx.getTurnForEvent(13)?.id).toBe(0);
  });
});

// Block 2 real-fixture tests: claude-code-1-error/ is a genuine Claude
// Code recording against the real Anthropic API with an invalid key —
// real envelope, real headers, real 401 status path. Anthropic SUCCESS
// content-block/streaming coverage stays on the in-tree mock scenarios
// (providers.test.ts) until issue #2 lands a real success fixture.
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import {
  asRequest,
  buildTurns,
  isErrorStatus,
  isProbeRequest,
  type TraceEvent,
} from "../lib/model";
import { errorInfo, foldBody } from "../lib/providers";

const FIX = join(__dirname, "..", "..", "test", "fixtures", "real", "claude-code-1-error");
const events: TraceEvent[] = readFileSync(join(FIX, "events.jsonl"), "utf8")
  .split("\n")
  .filter((l) => l.trim())
  .map((l) => JSON.parse(l) as TraceEvent);
const blob = (rel: string) => readFileSync(join(FIX, rel), "utf8");

const turns = buildTurns(events);

describe("claude-code-1-error fixture (real recording)", () => {
  it("has 735 events, 0 decode_error, 12 request/response pairs", () => {
    expect(events.length).toBe(735);
    expect(events.filter((e) => e["ev"] === "trace.decode_error").length).toBe(0);
    expect(events.filter((e) => e.ev === "llm.request").length).toBe(12);
    expect(events.filter((e) => e.ev === "llm.response").length).toBe(12);
  });

  it("builds 11 error turns and excludes the HEAD probe", () => {
    expect(turns.length).toBe(11);
    expect(turns.map((t) => t.id)).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
    expect(turns.every((t) => isErrorStatus(t.status))).toBe(true);
    expect(turns.every((t) => t.status === 401)).toBe(true);
    expect(turns.every((t) => t.error)).toBe(true);
    expect(turns.every((t) => t.provider === "anthropic")).toBe(true);

    const probeEvent = events.find((e) => e.ev === "llm.request" && e["id"] === 0)!;
    const probe = asRequest(probeEvent)!;
    expect(probe.method).toBe("HEAD");
    expect(probe.model).toBeNull();
    expect(probe.bytes).toBe(0);
    expect(isProbeRequest(probe)).toBe(true);
  });

  it("folds every real error body without throwing: empty content, no usage", () => {
    for (const t of turns) {
      const f = foldBody(t.provider, blob(t.resRel), false);
      expect(f).toEqual({ text: "", toolCalls: [], usage: null });
    }
    // and never throws on bodies that are not JSON at all
    expect(foldBody("anthropic", "not json{{", false)).toEqual({
      text: "",
      toolCalls: [],
      usage: null,
    });
  });

  it("parses the real Anthropic error envelope from every response body", () => {
    for (const t of turns) {
      expect(errorInfo(t.provider, blob(t.resRel))).toEqual({
        type: "authentication_error",
        message: "API key is invalid.",
      });
    }
    // Unrecognised shapes yield null — never a guessed message.
    expect(errorInfo("anthropic", '{"ok":true}')).toBeNull();
    expect(errorInfo("anthropic", "not json{{")).toBeNull();
    expect(errorInfo("openai", '{"type":"error","error":"str"}')).toBeNull();
    expect(errorInfo("openai", '{"error":{"message":"rate limited","type":"rate_limit"}}')).toEqual({
      type: "rate_limit",
      message: "rate limited",
    });
  });

  it("carries real redacted headers, provider and model", () => {
    const req = JSON.parse(blob(turns[0].reqRel)) as {
      headers: Record<string, string>;
      provider: string;
      method: string;
      body: string;
    };
    expect(req.method).toBe("POST");
    expect(req.provider).toBe("anthropic");
    expect(req.headers["x-api-key"]).toBe("REDACTED");
    expect(req.headers["Authorization"]).toBeUndefined();
    // the 11 real turns all ask for the same model; the probe carries none
    const modelled = events.filter((e) => e.ev === "llm.request" && e["model"] !== null);
    expect(modelled.length).toBe(11);
    expect(new Set(modelled.map((e) => e["model"]))).toEqual(new Set(["claude-opus-5-5"]));
  });
});

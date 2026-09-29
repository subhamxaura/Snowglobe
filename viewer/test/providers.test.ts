// Provider parser tests: inline SSE deltas + the committed toy-agent
// fixture blobs (bodies included, exactly what the viewer will render).
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import {
  costUsd,
  foldAnthropic,
  foldBody,
  foldOpenAi,
  parseSseFrames,
} from "../lib/providers";

const FIX = join(__dirname, "..", "..", "test", "fixtures", "real", "toy-agent-3turn", "llm");
const blob = (n: string) => readFileSync(join(FIX, n), "utf8");

describe("SSE framing", () => {
  it("splits frames and drops comments", () => {
    const frames = parseSseFrames(': comment\ndata: {"a":1}\n\ndata: [DONE]\n\n');
    expect(frames.length).toBe(2);
    expect(frames[0].data).toEqual(['{"a":1}']);
  });
});

describe("OpenAI folding", () => {
  it("folds a delta stream with tool calls and usage", () => {
    const sse = [
      'data: {"id":"1","choices":[{"delta":{"role":"assistant","content":"hi"}}]}',
      'data: {"id":"1","choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"write_file","arguments":"{\\"path\\":"}}]}}]}',
      'data: {"id":"1","choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\\"x\\"}"}}]}}]}',
      'data: {"id":"1","choices":[],"usage":{"prompt_tokens":100,"completion_tokens":20}}',
      "data: [DONE]",
    ].join("\n\n");
    const f = foldOpenAi(sse, true);
    expect(f.text).toBe("hi");
    expect(f.toolCalls).toEqual([{ id: "c1", name: "write_file", args: '{"path":"x"}' }]);
    expect(f.usage).toEqual({ input: 100, output: 20 });
  });

  it("reads the committed toy-agent responses", () => {
    for (const n of ["0000.res.json", "0001.res.json", "0002.res.json"]) {
      const f = foldOpenAi(blob(n), false);
      expect(f.usage).toBeNull(); // mock carries no usage block
    }
    // Dialogue order: run_command, then write_file + http_get, then done.
    const t0 = foldOpenAi(blob("0000.res.json"), false);
    expect(t0.toolCalls.length).toBe(1);
    expect(t0.toolCalls[0].name).toBe("run_command");
    const t1 = foldOpenAi(blob("0001.res.json"), false);
    expect(t1.toolCalls.length).toBe(2);
    expect(t1.toolCalls[0].name).toBe("write_file");
    expect(t1.toolCalls[0].args).toContain("note.txt");
    const t2 = foldOpenAi(blob("0002.res.json"), false);
    expect(t2.text).toBe("done");
    expect(t2.toolCalls).toEqual([]);
  });

  it("never throws on garbage", () => {
    expect(foldOpenAi("not json{{", false)).toEqual({ text: "", toolCalls: [], usage: null });
    expect(foldOpenAi("data: {\n\n", true).text).toBe("");
  });
});

describe("Anthropic folding", () => {
  it("folds text + input_json deltas", () => {
    const sse = [
      'data: {"type":"message_start","message":{"usage":{"input_tokens":50,"output_tokens":0}}}',
      'data: {"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"t1","name":"run_command"}}',
      'data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"working"}}',
      'data: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\\"cmd\\":"}}',
      'data: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"\\"ls\\"}"}}',
      'data: {"type":"message_delta","usage":{"input_tokens":50,"output_tokens":12}}',
    ].join("\n\n");
    const f = foldAnthropic(sse, true);
    expect(f.text).toBe("working");
    expect(f.toolCalls).toEqual([{ id: "t1", name: "run_command", args: '{"cmd":"ls"}' }]);
    expect(f.usage).toEqual({ input: 50, output: 12 });
  });

  it("reads full tool_use blocks", () => {
    const f = foldAnthropic(
      JSON.stringify({ content: [{ type: "tool_use", id: "t", name: "n", input: { a: 1 } }] }),
      false,
    );
    expect(f.toolCalls).toEqual([{ id: "t", name: "n", args: '{"a":1}' }]);
  });
});

describe("cost", () => {
  it("prices known models, nulls the rest", () => {
    expect(costUsd("gpt-4o-mini", { input: 1e6, output: 1e6 })).toBeCloseTo(0.75, 9);
    expect(costUsd("mock-model-1", { input: 5, output: 5 })).toBe(0);
    expect(costUsd("mystery-9", { input: 5, output: 5 })).toBeNull();
    expect(costUsd("gpt-4o", null)).toBeNull();
  });

  it("dispatches on provider", () => {
    const f = foldBody("anthropic", '{"content":[]}', false);
    expect(f).toEqual({ text: "", toolCalls: [], usage: null });
  });
});

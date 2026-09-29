// Provider parsing lives HERE (TypeScript), never in C++ (locked).
// OpenAI Chat Completions + Anthropic Messages: full bodies and SSE
// streams fold into {text, toolCalls, usage, cost}. Unknown shapes yield
// empty text and null usage/cost — never throw on agent traffic.

export interface FoldedUsage {
  input: number;
  output: number;
}

// Static $/1M-token table (approximate — verify with the provider).
// Unknown models cost null, never a guessed number.
const PRICE: Record<string, [number, number]> = {
  "gpt-4o": [2.5, 10],
  "gpt-4o-mini": [0.15, 0.6],
  "gpt-4-turbo": [10, 30],
  "claude-opus-4": [15, 75],
  "claude-sonnet-4": [3, 15],
  "claude-haiku-3-5": [0.8, 4],
  "mock-model-1": [0, 0],
};

export function costUsd(
  model: string | null,
  usage: FoldedUsage | null,
): number | null {
  if (!model || !usage) return null;
  const p = PRICE[model];
  if (!p) return null;
  return (usage.input * p[0] + usage.output * p[1]) / 1e6;
}

function safeJson(s: string): unknown {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

interface SseFrame {
  data: string[];
}

// Split a raw SSE stream into data frames (comments/blank lines dropped,
// "[DONE]" kept as a terminal marker the folders ignore).
export function parseSseFrames(text: string): SseFrame[] {
  const frames: SseFrame[] = [];
  for (const block of text.split(/\r?\n\r?\n/)) {
    const data: string[] = [];
    for (const line of block.split(/\r?\n/)) {
      if (line.startsWith("data:")) data.push(line.slice(5).trimStart());
    }
    if (data.length > 0) frames.push({ data });
  }
  return frames;
}

export interface Folded {
  text: string;
  toolCalls: { id: string; name: string; args: string }[];
  usage: FoldedUsage | null;
}

// OpenAI: non-streaming body or SSE delta stream (choices[].delta,
// tool_call deltas keyed by index, usage from any chunk carrying it).
export function foldOpenAi(bodyText: string, streamed: boolean): Folded {
  const out: Folded = { text: "", toolCalls: [], usage: null };
  const tc = new Map<number, { id: string; name: string; args: string }>();
  const visit = (msg: Record<string, unknown>) => {
    const content = msg["content"];
    if (typeof content === "string") out.text += content;
    const calls = msg["tool_calls"];
    if (Array.isArray(calls)) {
      for (const c of calls) {
        const cc = c as Record<string, unknown>;
        const fn = (cc["function"] ?? {}) as Record<string, unknown>;
        out.toolCalls.push({
          id: String(cc["id"] ?? ""),
          name: String(fn["name"] ?? ""),
          args: String(fn["arguments"] ?? ""),
        });
      }
    }
    const delta = msg["delta"] as Record<string, unknown> | undefined;
    if (delta && typeof delta === "object") {
      const dc = delta["content"];
      if (typeof dc === "string") out.text += dc;
      const dtc = delta["tool_calls"];
      if (Array.isArray(dtc)) {
        for (const c of dtc) {
          const cc = c as Record<string, unknown>;
          const idx = Number(cc["index"] ?? 0);
          let cur = tc.get(idx);
          if (!cur) {
            cur = { id: "", name: "", args: "" };
            tc.set(idx, cur);
          }
          if (typeof cc["id"] === "string") cur.id = cc["id"];
          const fn = (cc["function"] ?? {}) as Record<string, unknown>;
          if (typeof fn["name"] === "string") cur.name = fn["name"];
          if (typeof fn["arguments"] === "string") cur.args += fn["arguments"];
        }
      }
    }
    const u = msg["usage"] as Record<string, unknown> | undefined;
    if (u && typeof u === "object") {
      const pi = u["prompt_tokens"];
      const co = u["completion_tokens"];
      if (typeof pi === "number" && typeof co === "number") {
        out.usage = { input: pi, output: co };
      }
    }
  };
  if (!streamed) {
    const body = safeJson(bodyText) as Record<string, unknown> | null;
    const choices = body?.["choices"];
    if (Array.isArray(choices)) {
      for (const ch of choices) {
        const m = (ch as Record<string, unknown>)["message"];
        if (m && typeof m === "object") visit(m as Record<string, unknown>);
      }
    }
    const u = body?.["usage"] as Record<string, unknown> | undefined;
    if (u && typeof u === "object") visit({ usage: u });
  } else {
    for (const f of parseSseFrames(bodyText)) {
      for (const d of f.data) {
        if (d === "[DONE]") continue;
        const obj = safeJson(d) as Record<string, unknown> | null;
        if (!obj || typeof obj !== "object") continue;
        const choices = obj["choices"];
        if (Array.isArray(choices)) {
          for (const ch of choices) {
            const dd = (ch as Record<string, unknown>)["delta"];
            if (dd && typeof dd === "object") {
              visit({ delta: dd });
            } else {
              visit(ch as Record<string, unknown>);
            }
            const u = (ch as Record<string, unknown>)["usage"];
            if (u && typeof u === "object") visit({ usage: u });
          }
        }
        const u = obj["usage"];
        if (u && typeof u === "object") visit({ usage: u });
      }
    }
  }
  for (const c of tc.values()) {
    if (c.id || c.name || c.args) out.toolCalls.push(c);
  }
  return out;
}

// Anthropic: content blocks (text/tool_use) or SSE deltas
// (content_block_delta: text_delta/input_json_delta, message_delta usage).
export function foldAnthropic(bodyText: string, streamed: boolean): Folded {
  const out: Folded = { text: "", toolCalls: [], usage: null };
  const jsonDeltas = new Map<number, { id: string; name: string; args: string }>();
  const visitContent = (blocks: unknown) => {
    if (!Array.isArray(blocks)) return;
    for (const b of blocks) {
      const bb = b as Record<string, unknown>;
      if (bb["type"] === "text" && typeof bb["text"] === "string") {
        out.text += bb["text"];
      } else if (bb["type"] === "tool_use") {
        out.toolCalls.push({
          id: String(bb["id"] ?? ""),
          name: String(bb["name"] ?? ""),
          args: JSON.stringify(bb["input"] ?? {}),
        });
      }
    }
  };
  const visitUsage = (u: unknown) => {
    const uu = u as Record<string, unknown>;
    const inp = uu["input_tokens"];
    const outp = uu["output_tokens"];
    if (typeof inp === "number" && typeof outp === "number") {
      out.usage = { input: inp, output: outp };
    }
  };
  if (!streamed) {
    const body = safeJson(bodyText) as Record<string, unknown> | null;
    if (body && typeof body === "object") {
      visitContent(body["content"]);
      if (body["usage"]) visitUsage(body["usage"]);
    }
  } else {
    for (const f of parseSseFrames(bodyText)) {
      for (const d of f.data) {
        if (d === "[DONE]") continue;
        const ev = safeJson(d) as Record<string, unknown> | null;
        if (!ev || typeof ev !== "object") continue;
        const t = ev["type"];
        if (t === "content_block_start") {
          const b = (ev["content_block"] ?? {}) as Record<string, unknown>;
          if (b["type"] === "tool_use") {
            jsonDeltas.set(Number(ev["index"] ?? 0), {
              id: String(b["id"] ?? ""),
              name: String(b["name"] ?? ""),
              args: "",
            });
          }
        } else if (t === "content_block_delta") {
          const idx = Number(ev["index"] ?? 0);
          const delta = (ev["delta"] ?? {}) as Record<string, unknown>;
          if (delta["type"] === "text_delta" && typeof delta["text"] === "string") {
            out.text += delta["text"];
          } else if (
            delta["type"] === "input_json_delta" &&
            typeof delta["partial_json"] === "string"
          ) {
            const cur = jsonDeltas.get(idx) ?? { id: "", name: "", args: "" };
            cur.args += delta["partial_json"];
            jsonDeltas.set(idx, cur);
          }
        } else if (t === "message_delta") {
          if (ev["usage"]) visitUsage(ev["usage"]);
        } else if (t === "message_start") {
          const m = (ev["message"] ?? {}) as Record<string, unknown>;
          if (m["usage"]) visitUsage(m["usage"]);
        }
      }
    }
    for (const c of jsonDeltas.values()) {
      if (c.id || c.name || c.args) out.toolCalls.push(c);
    }
  }
  return out;
}

export function foldBody(
  provider: string,
  bodyText: string,
  streamed: boolean,
): Folded {
  if (provider === "anthropic") return foldAnthropic(bodyText, streamed);
  return foldOpenAi(bodyText, streamed);
}

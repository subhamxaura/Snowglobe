// OpenAI Chat Completions: non-streaming body or SSE delta stream
// (choices[].delta, tool_call deltas keyed by index, usage from any
// chunk carrying it). Unknown shapes yield empty text and null usage —
// never throw on agent traffic.
import { parseSseFrames } from "./sse";
import type { ErrorInfo, Folded } from "./types";
import { safeJson } from "./types";

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

// OpenAI error envelope: {"error":{"message":...,"type":...,"code":...}}.
export function openaiErrorInfo(bodyText: string): ErrorInfo | null {
  const body = safeJson(bodyText);
  if (!body || typeof body !== "object") return null;
  const err = (body as Record<string, unknown>)["error"];
  if (!err || typeof err !== "object") return null;
  const e = err as Record<string, unknown>;
  if (typeof e["message"] !== "string") return null;
  return {
    type: typeof e["type"] === "string" ? e["type"] : "error",
    message: e["message"],
  };
}

// Anthropic Messages: content blocks (text/tool_use) or SSE deltas
// (content_block_delta: text_delta/input_json_delta, message_delta
// usage). Unknown shapes yield empty text and null usage — never throw
// on agent traffic. Error envelopes come from the real recording in
// test/fixtures/real/claude-code-1-error/ (401 authentication_error).
import { parseSseFrames } from "./sse";
import type { ErrorInfo, Folded } from "./types";
import { safeJson } from "./types";

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

// Anthropic error envelope: {"type":"error","error":{"type":...,
// "message":...}} — the real shape returned for 401/429/5xx.
export function anthropicErrorInfo(bodyText: string): ErrorInfo | null {
  const body = safeJson(bodyText);
  if (!body || typeof body !== "object") return null;
  const o = body as Record<string, unknown>;
  if (o["type"] !== "error") return null;
  const e = o["error"];
  if (!e || typeof e !== "object") return null;
  const ee = e as Record<string, unknown>;
  if (typeof ee["message"] !== "string") return null;
  return {
    type: typeof ee["type"] === "string" ? ee["type"] : "error",
    message: ee["message"],
  };
}

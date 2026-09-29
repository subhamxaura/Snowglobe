import { useEffect, useMemo, useState } from "react";
import { foldBody, costUsd } from "../lib/providers";
import { loadText } from "../lib/load";
import { fmtBytes, fmtCost, fmtMs } from "../lib/format";
import type { LlmTurn, TraceEvent } from "../lib/types";

export interface TurnVM extends LlmTurn {
  reqBody: string;
  resBody: string;
  text: string;
  tools: { id: string; name: string; args: string }[];
  usage: { input: number; output: number } | null;
  cost: number | null;
  linked: TraceEvent[];
}

// Turns join llm.request/response by id, fold bodies through the provider
// parsers, and link side effects: any fs.open(write) whose path appears in
// a tool-call's arguments (e.g. write_file under turn 2).
export function useTurns(events: TraceEvent[]): {
  turns: TurnVM[];
  loading: boolean;
} {
  const [blobs, setBlobs] = useState<Record<string, string>>({});
  const [loading, setLoading] = useState(false);
  const pairs = useMemo(() => {
    const reqs = new Map<number, TraceEvent>();
    const resps = new Map<number, TraceEvent>();
    for (const e of events) {
      if (e.ev === "llm.request") reqs.set(Number(e["id"]), e);
      else if (e.ev === "llm.response") resps.set(Number(e["id"]), e);
    }
    return [...reqs.keys()]
      .filter((id) => resps.has(id))
      .sort((a, b) => a - b)
      .map((id) => ({ id, req: reqs.get(id)!, res: resps.get(id)! }));
  }, [events]);
  useEffect(() => {
    let live = true;
    setLoading(true);
    (async () => {
      const next: Record<string, string> = {};
      await Promise.all(
        pairs.flatMap(({ res }) => {
          const rr = String(res["req"] ?? "");
          const sr = String(res["res"] ?? "");
          return [rr, sr].map(async (rel) => {
            if (!rel || next[rel] !== undefined) return;
            try {
              next[rel] = await loadText(rel);
            } catch {
              next[rel] = "";
            }
          });
        }),
      );
      if (live) {
        setBlobs(next);
        setLoading(false);
      }
    })();
    return () => {
      live = false;
    };
  }, [pairs]);
  const turns = useMemo<TurnVM[]>(
    () =>
      pairs.map(({ id, req, res }) => {
        const provider = String(req["provider"] ?? "unknown");
        const model =
          typeof req["model"] === "string" ? (req["model"] as string) : null;
        const stream = req["stream"] === true;
        const reqBody = blobs[String(res["req"] ?? "")] ?? "";
        const resBody = blobs[String(res["res"] ?? "")] ?? "";
        const folded = foldBody(provider, resBody, stream || looksStreamed(resBody));
        const linked = linkSideEffects(events, folded.toolCalls.map((t) => t.args).join("\n"));
        return {
          id,
          provider,
          model,
          stream,
          status: Number(res["status"] ?? 0),
          bytes: Number(res["bytes"] ?? 0),
          ttfb_ms: typeof res["ttfb_ms"] === "number" ? (res["ttfb_ms"] as number) : null,
          total_ms: Number(res["total_ms"] ?? 0),
          chunks: Number(res["chunk_count"] ?? 0),
          truncated: res["truncated"] === true,
          reqRel: String(res["req"] ?? ""),
          resRel: String(res["res"] ?? ""),
          reqBody,
          resBody,
          text: folded.text,
          tools: folded.toolCalls,
          usage: folded.usage,
          cost: costUsd(model, folded.usage),
          linked,
        };
      }),
    [pairs, blobs, events],
  );
  return { turns, loading };
}

function looksStreamed(body: string): boolean {
  return body.includes("data:");
}

// Side-effect linkage: fs.open(write) events whose path is named inside
// tool arguments, plus proc.exec events sharing the turn window is out of
// scope — path linkage is exact and explainable.
function linkSideEffects(events: TraceEvent[], haystack: string): TraceEvent[] {
  if (!haystack) return [];
  return events.filter(
    (e) =>
      e.ev === "fs.open" &&
      e["write"] === true &&
      typeof e["path"] === "string" &&
      haystack.includes(e["path"] as string),
  );
}

export default function Turns({ events }: { events: TraceEvent[] }) {
  const { turns, loading } = useTurns(events);
  if (turns.length === 0) {
    return <p className="muted">no LLM turns in this trace</p>;
  }
  return (
    <div data-testid="turns">
      {loading && <p className="muted">loading blobs…</p>}
      {turns.map((t) => (
        <section
          key={t.id}
          data-testid={`turn-${t.id}`}
          style={{ border: "1px solid var(--border)", margin: "12px 0", padding: 12 }}
        >
          <h3>
            turn {t.id + 1} <span className="muted">· {t.provider}</span>
          </h3>
          <p className="muted">
            model {t.model ?? "—"} · status {t.status} ·{" "}
            {fmtBytes(t.bytes)} · ttfb {fmtMs(t.ttfb_ms)} · total{" "}
            {fmtMs(t.total_ms)} · {t.chunks} chunks · cost {fmtCost(t.cost)}
            {t.truncated ? " · TRUNCATED" : ""}
          </p>
          {t.text && <pre data-testid={`turn-text-${t.id}`}>{t.text}</pre>}
          <details>
            <summary className="muted">request body</summary>
            <pre>{t.reqBody.slice(0, 2000)}</pre>
          </details>
          {t.tools.map((c, i) => (
            <div key={i} data-testid={`tool-${t.id}-${i}`}>
              <b>
                {c.name || "(unnamed tool)"} <span className="muted">{c.id}</span>
              </b>
              <pre>{c.args}</pre>
            </div>
          ))}
          {t.linked.length > 0 && (
            <div data-testid={`linked-${t.id}`}>
              <b>linked side effects</b>
              <ul>
                {t.linked.map((e) => (
                  <li key={e.seq}>
                    {e.ev} {String(e["path"] ?? "")}
                  </li>
                ))}
              </ul>
            </div>
          )}
        </section>
      ))}
    </div>
  );
}

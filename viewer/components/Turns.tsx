import { useEffect, useMemo, useState } from "react";
import { foldBody, costUsd, errorInfo } from "../lib/providers";
import { TurnIndex, buildTurns, isErrorStatus, seqOf } from "../lib/model";
import type { LinksDoc } from "../lib/model";
import { loadLinks, loadText } from "../lib/load";
import { fmtBytes, fmtCost, fmtMs } from "../lib/format";
import type { LlmTurn, TraceEvent } from "../lib/types";

export interface LinkedEvent {
  event: TraceEvent;
  basis: string; // window | lineage | argv-match (sidecar) or heuristic
  seq: number; // sidecar order key (fixtures have no per-event seq)
}

export interface TurnVM extends LlmTurn {
  reqBody: string;
  resBody: string;
  text: string;
  tools: { id: string; name: string; args: string }[];
  usage: { input: number; output: number } | null;
  cost: number | null;
  err: { type: string; message: string } | null;
  linked: LinkedEvent[];
  unattributed: { seq: number; reason: string }[];
  linkSource: "sidecar" | "heuristic";
}

// Turns join llm.request/response by id, fold bodies through the provider
// parsers, and attribute side effects through the links.json sidecar when
// present (TurnIndex: sidecar truth, seq heuristic only where silent).
// Without links.json the old path-substring heuristic applies and the UI
// says so — never a guess presented as fact.
export function useTurns(events: TraceEvent[]): {
  turns: TurnVM[];
  loading: boolean;
} {
  const [blobs, setBlobs] = useState<Record<string, string>>({});
  const [links, setLinks] = useState<LinksDoc | null>(null);
  const [loading, setLoading] = useState(false);
  const pairs = useMemo(() => {
    const reqs = new Map<number, TraceEvent>();
    const resps = new Map<number, TraceEvent>();
    for (const e of events) {
      if (e.ev === "llm.request") reqs.set(Number(e["id"]), e);
      else if (e.ev === "llm.response") resps.set(Number(e["id"]), e);
    }
    // buildTurns owns the turn rules (every non-probe llm.response is a
    // turn, HEAD/model-less probes never are) — the view must agree with
    // /api/summary and model.ts, never re-derive its own pairing.
    return buildTurns(events)
      .filter((t) => reqs.has(t.id) && resps.has(t.id))
      .map((t) => ({ id: t.id, req: reqs.get(t.id)!, res: resps.get(t.id)! }));
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
  useEffect(() => {
    let live = true;
    loadLinks().then((d) => {
      if (live) setLinks(d);
    });
    return () => {
      live = false;
    };
  }, []);
  const index = useMemo(() => new TurnIndex(events, links), [events, links]);
  const bySeq = useMemo(() => {
    const m = new Map<number, TraceEvent>();
    events.forEach((e, i) => {
      if (!m.has(seqOf(e, i))) m.set(seqOf(e, i), e);
    });
    return m;
  }, [events]);
  const turns = useMemo<TurnVM[]>(
    () =>
      pairs.map(({ id, req, res }) => {
        const provider = String(req["provider"] ?? "unknown");
        const model =
          typeof req["model"] === "string" ? (req["model"] as string) : null;
        const stream = req["stream"] === true;
        const reqBody = blobs[String(res["req"] ?? "")] ?? "";
        const resBody = blobs[String(res["res"] ?? "")] ?? "";
        const status = Number(res["status"] ?? 0);
        const folded = foldBody(provider, resBody, stream || looksStreamed(resBody));
        const err = isErrorStatus(status) ? errorInfo(provider, resBody) : null;
        const sidecar = links?.turns.find((lt) => lt.turn === id) ?? null;
        let linked: LinkedEvent[];
        let unattributed: { seq: number; reason: string }[] = [];
        if (sidecar) {
          linked = sidecar.attributed.flatMap((a) => {
            const event = bySeq.get(a.seq);
            return event ? [{ event, basis: a.basis, seq: a.seq }] : [];
          });
          unattributed = sidecar.unattributed.map((u) => ({ seq: u.seq, reason: u.reason }));
        } else {
          linked = linkSideEffects(events, folded.toolCalls.map((t) => t.args).join("\n"));
        }
        return {
          id,
          provider,
          model,
          stream,
          status,
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
          err,
          linked,
          unattributed,
          linkSource: index.source,
        };
      }),
    [pairs, blobs, events, links, index, bySeq],
  );
  return { turns, loading };
}

function looksStreamed(body: string): boolean {
  return body.includes("data:");
}

// Side-effect linkage fallback (no links.json): fs.open(write) events
// whose path is named inside tool arguments. Exact and explainable, but
// a heuristic — the UI labels it as such.
function linkSideEffects(events: TraceEvent[], haystack: string): LinkedEvent[] {
  if (!haystack) return [];
  const out: LinkedEvent[] = [];
  events.forEach((event, i) => {
    if (
      event.ev === "fs.open" &&
      event["write"] === true &&
      typeof event["path"] === "string" &&
      haystack.includes(event["path"] as string)
    ) {
      out.push({ event, basis: "heuristic", seq: seqOf(event, i) });
    }
  });
  return out;
}

export default function Turns({ events }: { events: TraceEvent[] }) {
  const { turns, loading } = useTurns(events);
  if (turns.length === 0) {
    return <p className="muted">no LLM turns in this trace</p>;
  }
  return (
    <div data-testid="turns">
      {turns.length > 0 && (
        <p className="muted" data-testid="link-source">
          linkage: {turns[0].linkSource}
        </p>
      )}
      {loading && <p className="muted">loading blobs…</p>}
      {turns.map((t, i) => (
        <section
          key={t.id}
          data-testid={`turn-${t.id}`}
          style={{ border: "1px solid var(--border)", margin: "12px 0", padding: 12 }}
        >
          <h3>
            turn {i + 1} <span className="muted">· {t.provider}</span>
          </h3>
          <p className="muted">
            model {t.model ?? "—"} · status {t.status} ·{" "}
            {fmtBytes(t.bytes)} · ttfb {fmtMs(t.ttfb_ms)} · total{" "}
            {fmtMs(t.total_ms)} · {t.chunks} chunks · cost {fmtCost(t.cost)}
            {t.truncated ? " · TRUNCATED" : ""}
          </p>
          {isErrorStatus(t.status) && (
            <p className="error" data-testid={`turn-error-${t.id}`}>
              <b>HTTP {t.status}</b>
              {t.err ? ` · ${t.err.type}: ${t.err.message}` : ""}
            </p>
          )}
          {t.text && <pre data-testid={`turn-text-${t.id}`}>{t.text}</pre>}
          <details>
            <summary className="muted">request body</summary>
            <pre>{t.reqBody.slice(0, 2000)}</pre>
          </details>
          <details data-testid={`raw-body-${t.id}`}>
            <summary className="muted">response body</summary>
            <pre>{t.resBody.slice(0, 50000)}</pre>
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
              <b>linked side effects</b>{" "}
              {[...new Set(t.linked.map((l) => l.basis))].map((b) => (
                <span key={b} data-testid={`basis-${t.id}-${b}`}>
                  [{b}]
                </span>
              ))}
              <ul>
                {t.linked.map((l) => (
                  <li key={l.seq} data-testid={`linked-ev-${t.id}-${l.seq}`}>
                    {l.event.ev} {String(l.event["path"] ?? "")} [{l.basis}]
                  </li>
                ))}
              </ul>
            </div>
          )}
          {t.unattributed.length > 0 && (
            <div data-testid={`unattributed-${t.id}`}>
              <b>unattributed</b>
              <ul>
                {t.unattributed.map((u) => (
                  <li key={u.seq}>
                    seq {u.seq} · {u.reason}
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

import { useEffect, useMemo, useState } from "react";
import VList from "./VList";
import { TurnIndex, seqOf } from "../lib/model";
import type { LinksDoc, Turn } from "../lib/model";
import { loadLinks } from "../lib/load";
import type { TraceEvent } from "../lib/types";

const KINDS = [
  "all",
  "proc",
  "fs",
  "net",
  "llm",
  "run",
  "trace",
] as const;

// Per-turn lane colors (cycled past 8 turns); pre-turn is always grey.
// Inline styles: no theme dependency, readable on light and dark.
const LANE_COLORS = [
  "#2563eb",
  "#059669",
  "#d97706",
  "#7c3aed",
  "#db2777",
  "#0891b2",
  "#65a30d",
  "#dc2626",
];
const PRE_COLOR = "#6b7280";

interface LaneRow {
  e: TraceEvent;
  idx: number;
  seq: number;
  turn: Turn | null; // owning turn (sidecar exact or span heuristic)
  turnPos: number; // 1-based position in TurnIndex.list (matches Turns tab)
  basis: string | null; // window | lineage | argv-match | heuristic
  reason: string | null; // unattributed reason (pre-turn, …)
}

// Lane assignment, single-sourced on TurnIndex (identical rules to the
// Turns tab): the links.json sidecar wins where it speaks (exact seq +
// basis + explicit unattributed reasons); unlisted seqs keep the
// request-anchored span heuristic with basis "heuristic"; seqs before
// the first turn land in the grey pre-turn lane, never dropped.
function laneOf(
  index: TurnIndex,
  posOf: Map<number, number>,
  e: TraceEvent,
  idx: number,
): LaneRow {
  const seq = seqOf(e, idx);
  const link = index.linkOf(seq);
  if (link !== null && "turn" in link) {
    return {
      e,
      idx,
      seq,
      turn: link.turn,
      turnPos: posOf.get(link.turn.id) ?? 0,
      basis: link.basis,
      reason: null,
    };
  }
  if (link !== null && "unattributed" in link) {
    return { e, idx, seq, turn: null, turnPos: 0, basis: null, reason: link.unattributed };
  }
  const turn = index.getTurnForEvent(seq);
  if (turn) {
    return {
      e,
      idx,
      seq,
      turn,
      turnPos: posOf.get(turn.id) ?? 0,
      basis: "heuristic",
      reason: null,
    };
  }
  return { e, idx, seq, turn: null, turnPos: 0, basis: null, reason: "pre-turn" };
}

// Every event, newest last, filterable by family and by turn lane. The
// 60fps path: VList renders a window, never the whole trace; lane
// assignment is one linear memo pass, not per-row work.
export default function Timeline({ events }: { events: TraceEvent[] }) {
  const [kind, setKind] = useState<(typeof KINDS)[number]>("all");
  const [query, setQuery] = useState("");
  const [lane, setLane] = useState<string>("all");
  const [links, setLinks] = useState<LinksDoc | null>(null);
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
  const posOf = useMemo(() => {
    const m = new Map<number, number>();
    index.list.forEach((t, i) => m.set(t.id, i + 1));
    return m;
  }, [index]);
  const laned = useMemo(
    () => events.map((e, idx) => laneOf(index, posOf, e, idx)),
    [events, index, posOf],
  );
  const laneCounts = useMemo(() => {
    const m = new Map<string, number>();
    for (const r of laned) {
      const k = r.turn ? `turn:${r.turn.id}` : "pre";
      m.set(k, (m.get(k) ?? 0) + 1);
    }
    return m;
  }, [laned]);
  const rows = useMemo(() => {
    const q = query.toLowerCase();
    return laned.filter((r) => {
      const e = r.e;
      const fam = e.ev.split(".")[0];
      if (kind !== "all" && fam !== kind && e.ev !== kind) return false;
      if (lane === "pre" && r.turn !== null) return false;
      if (lane.startsWith("turn:") && (!r.turn || `turn:${r.turn.id}` !== lane)) {
        return false;
      }
      if (q && !JSON.stringify(e).toLowerCase().includes(q)) return false;
      return true;
    });
  }, [laned, kind, query, lane]);
  return (
    <div>
      <div className="tabs" role="tablist" aria-label="event family">
        {KINDS.map((k) => (
          <button
            key={k}
            aria-selected={kind === k}
            onClick={() => setKind(k)}
          >
            {k}
          </button>
        ))}
        <input
          aria-label="filter events"
          placeholder="filter…"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          style={{ marginLeft: 8 }}
        />
      </div>
      <p className="muted" data-testid="timeline-link-source">
        linkage: {index.source}
      </p>
      <div role="tablist" aria-label="turn lane">
        <button
          data-testid="lane-all"
          aria-selected={lane === "all"}
          onClick={() => setLane("all")}
        >
          all lanes
        </button>
        <button
          data-testid="lane-pre"
          aria-selected={lane === "pre"}
          onClick={() => setLane("pre")}
          title="events before the first turn (sidecar pre-turn bucket or span heuristic)"
        >
          pre-turn ({laneCounts.get("pre") ?? 0})
        </button>
        {index.list.map((t) => (
          <button
            key={t.id}
            data-testid={`lane-${t.id}`}
            aria-selected={lane === `turn:${t.id}`}
            onClick={() => setLane(`turn:${t.id}`)}
            title={`turn ${posOf.get(t.id)} · ${t.provider} · status ${t.status}`}
          >
            <span
              style={{
                display: "inline-block",
                width: 10,
                height: 10,
                background: LANE_COLORS[((posOf.get(t.id) ?? 1) - 1) % LANE_COLORS.length],
                marginRight: 4,
              }}
            />
            T{posOf.get(t.id)} ({laneCounts.get(`turn:${t.id}`) ?? 0})
          </button>
        ))}
      </div>
      <p className="muted" data-testid="timeline-count">
        {rows.length} / {events.length} events
      </p>
      <VList
        items={rows}
        rowHeight={30}
        height={480}
        keyOf={(r) => r.seq * 100000 + r.idx}
        renderRow={(r) => {
          const color = r.turn
            ? LANE_COLORS[(r.turnPos - 1) % LANE_COLORS.length]
            : PRE_COLOR;
          const title = r.turn
            ? `turn T${r.turnPos} (llm id ${r.turn.id}) · basis ${r.basis}`
            : `pre-turn · ${r.reason ?? "unattributed"}`;
          return (
            <div
              title={title}
              style={{
                padding: "4px 8px",
                whiteSpace: "nowrap",
                borderLeft: `4px solid ${color}`,
              }}
            >
              <span
                data-testid={r.turn ? `lane-chip-${r.turn.id}` : "lane-chip-pre"}
                style={{
                  display: "inline-block",
                  minWidth: 34,
                  textAlign: "center",
                  color: "#fff",
                  background: color,
                  marginRight: 6,
                  padding: "0 4px",
                }}
              >
                {r.turn ? `T${r.turnPos}` : "pre"}
              </span>
              <span className="muted">#{r.seq}</span> <b>{r.e.ev}</b>{" "}
              <span className="muted">
                pid={String(r.e.pid)} tid={String(r.e.tid)} t={r.e.t_ms}ms
              </span>{" "}
              {r.basis && (
                <span data-testid={`basis-${r.seq}`} className="muted">
                  [{r.basis}]
                </span>
              )}
              {r.reason && <span className="muted">({r.reason})</span>}
            </div>
          );
        }}
      />
    </div>
  );
}

import { useMemo, useState } from "react";
import VList from "./VList";
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

// Every event, newest last, filterable by family. The 60fps path: VList
// renders a window, never the whole trace.
export default function Timeline({ events }: { events: TraceEvent[] }) {
  const [kind, setKind] = useState<(typeof KINDS)[number]>("all");
  const [query, setQuery] = useState("");
  const rows = useMemo(() => {
    const q = query.toLowerCase();
    return events.filter((e) => {
      const fam = e.ev.split(".")[0];
      if (kind !== "all" && fam !== kind && e.ev !== kind) return false;
      if (q && !JSON.stringify(e).toLowerCase().includes(q)) return false;
      return true;
    });
  }, [events, kind, query]);
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
      <p className="muted" data-testid="timeline-count">
        {rows.length} / {events.length} events
      </p>
      <VList
        items={rows}
        rowHeight={26}
        height={480}
        keyOf={(e) => e.seq}
        renderRow={(e) => (
          <div style={{ padding: "3px 8px", whiteSpace: "nowrap" }}>
            <span className="muted">#{e.seq}</span> <b>{e.ev}</b>{" "}
            <span className="muted">
              pid={String(e.pid)} tid={String(e.tid)} t={e.t_ms}ms
            </span>
          </div>
        )}
      />
    </div>
  );
}

import { useMemo, useState } from "react";
import VList from "./VList";
import type { TraceEvent } from "../lib/types";

// Substring search across the whole event stream (JSON-encoded per event).
export default function Search({ events }: { events: TraceEvent[] }) {
  const [q, setQ] = useState("");
  const rows = useMemo(() => {
    const needle = q.toLowerCase();
    if (!needle) return [];
    return events.filter((e) =>
      JSON.stringify(e).toLowerCase().includes(needle),
    );
  }, [events, q]);
  return (
    <div>
      <input
        aria-label="search trace"
        placeholder="search all events…"
        value={q}
        onChange={(e) => setQ(e.target.value)}
        style={{ width: "100%", padding: 6 }}
      />
      {q && (
        <p className="muted" data-testid="search-count">
          {rows.length} matches
        </p>
      )}
      <VList
        items={rows}
        rowHeight={26}
        height={440}
        keyOf={(e) => e.seq}
        renderRow={(e) => (
          <div style={{ padding: "3px 8px", whiteSpace: "nowrap" }}>
            <span className="muted">#{e.seq}</span> <b>{e.ev}</b>
          </div>
        )}
      />
    </div>
  );
}

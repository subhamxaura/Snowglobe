import { useMemo } from "react";
import VList from "./VList";
import type { TraceEvent } from "../lib/types";

interface FileRow {
  path: string;
  writes: number;
  reads: number;
  unlinks: number;
  renames: number;
}

// Files touched, grouped by path: writes/reads/unlinks/renames.
export default function Files({ events }: { events: TraceEvent[] }) {
  const rows = useMemo(() => {
    const m = new Map<string, FileRow>();
    const row = (path: string): FileRow => {
      let r = m.get(path);
      if (!r) {
        r = { path, writes: 0, reads: 0, unlinks: 0, renames: 0 };
        m.set(path, r);
      }
      return r;
    };
    for (const e of events) {
      if (e.ev === "fs.open") {
        const r = row(String(e["path"] ?? "?"));
        if (e["write"] === true) r.writes++;
        else r.reads++;
      } else if (e.ev === "fs.unlink" || e.ev === "fs.rmdir") {
        row(String(e["path"] ?? "?")).unlinks++;
      } else if (e.ev === "fs.rename") {
        row(String(e["from"] ?? "?")).renames++;
        row(String(e["to"] ?? "?")).renames++;
      }
    }
    return [...m.values()].sort((a, b) => b.writes - a.writes || b.reads - a.reads);
  }, [events]);
  return (
    <div data-testid="files">
      <p className="muted">{rows.length} paths</p>
      <VList
        items={rows}
        rowHeight={26}
        height={480}
        keyOf={(r) => r.path}
        renderRow={(r) => (
          <div style={{ padding: "3px 8px", whiteSpace: "nowrap" }}>
            {r.path}{" "}
            <span className="muted">
              w:{r.writes} r:{r.reads} del:{r.unlinks} mv:{r.renames}
            </span>
          </div>
        )}
      />
    </div>
  );
}

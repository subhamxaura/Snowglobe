import { useMemo } from "react";
import VList from "./VList";
import { fmtBytes, fmtMs } from "../lib/format";
import type { TraceEvent } from "../lib/types";

// Network: socket events plus the LLM exchanges that caused most of them.
export default function Network({ events }: { events: TraceEvent[] }) {
  const rows = useMemo(
    () =>
      events.filter(
        (e) =>
          e.ev === "net.connect" ||
          e.ev === "net.sendto" ||
          e.ev === "net.bind" ||
          e.ev === "llm.request" ||
          e.ev === "llm.response",
      ),
    [events],
  );
  return (
    <div data-testid="network">
      <p className="muted">{rows.length} network/LLM events</p>
      <VList
        items={rows}
        rowHeight={26}
        height={480}
        keyOf={(e) => e.seq}
        renderRow={(e) => (
          <div style={{ padding: "3px 8px", whiteSpace: "nowrap" }}>
            <span className="muted">#{e.seq}</span> <b>{e.ev}</b>{" "}
            {e.ev === "llm.request"
              ? `${String(e["method"])} ${String(e["path"])} → ${String(e["provider"])}`
              : e.ev === "llm.response"
                ? `#${String(e["id"])} ${e["status"]} ${fmtBytes(Number(e["bytes"] ?? 0))} ttfb ${fmtMs(typeof e["ttfb_ms"] === "number" ? (e["ttfb_ms"] as number) : null)}`
                : `${String(e["addr"] ?? "")} ${e["ok"] === false ? "FAILED" : "ok"}`}
          </div>
        )}
      />
    </div>
  );
}

import { useMemo } from "react";
import type { ProcNode, TraceEvent } from "../lib/types";

// Process tree from proc.start (ppid threading, thread:true stays nested
// under its tgid), annotated with proc.exec argv and proc.exit codes.
export function buildTree(events: TraceEvent[]): ProcNode[] {
  const nodes = new Map<string, ProcNode>();
  const get = (pid: string): ProcNode => {
    let n = nodes.get(pid);
    if (!n) {
      n = {
        pid,
        ppid: "",
        thread: false,
        root: false,
        execs: [],
        exit: null,
        children: [],
      };
      nodes.set(pid, n);
    }
    return n;
  };
  for (const e of events) {
    const pid = String(e.pid);
    if (e.ev === "proc.start") {
      const n = get(pid);
      n.ppid = String(e["ppid"] ?? "");
      n.thread = e["thread"] === true;
      n.root = e["root"] === true;
    } else if (e.ev === "proc.exec" || e.ev === "proc.exec_failed") {
      get(pid).execs.push(e);
    } else if (e.ev === "proc.exit") {
      get(pid).exit = e;
    }
  }
  const roots: ProcNode[] = [];
  for (const n of nodes.values()) {
    if (n.ppid && nodes.has(n.ppid) && n.ppid !== n.pid) {
      nodes.get(n.ppid)!.children.push(n);
    } else {
      roots.push(n);
    }
  }
  return roots;
}

function Row({ n, depth }: { n: ProcNode; depth: number }) {
  const cmd = n.execs
    .map((e) => ((e["argv"] as string[]) ?? [String(e["path"] ?? "?")]).join(" "))
    .join(" → ");
  const exit = n.exit;
  const code =
    exit && typeof exit["code"] === "number"
      ? `exit ${exit["code"]}`
      : exit && exit["vanished"] === true
        ? "vanished"
        : "running";
  return (
    <div>
      <div style={{ paddingLeft: depth * 18 }}>
        <span className="muted">{n.pid}</span> {n.thread ? "[thread] " : ""}
        {cmd || "(no exec)"} <span className="muted">· {code}</span>
      </div>
      {n.children.map((c) => (
        <Row key={`${c.pid}-${c.thread}`} n={c} depth={depth + 1} />
      ))}
    </div>
  );
}

export default function Processes({ events }: { events: TraceEvent[] }) {
  const roots = useMemo(() => buildTree(events), [events]);
  return (
    <div data-testid="processes">
      {roots.map((r) => (
        <Row key={r.pid} n={r} depth={0} />
      ))}
    </div>
  );
}

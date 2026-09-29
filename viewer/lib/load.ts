// Trace loading: streams events.jsonl in chunks (keeps the UI alive on
// 50 MB traces), parses incrementally, reports progress. Blobs fetch
// on demand per turn (small count, never the whole llm/ dir).
import type { Manifest, TraceEvent } from "./types";

export interface LoadProgress {
  bytes: number;
  events: number;
  done: boolean;
}

export async function loadManifest(): Promise<Manifest> {
  const r = await fetch("/trace/manifest.json");
  if (!r.ok) throw new Error(`manifest: HTTP ${r.status}`);
  return (await r.json()) as Manifest;
}

// Reads the full stream but yields progress per 1 MiB so the status line
// stays live; returns all events. A 50 MB file parses in ~1-2 s.
export async function loadEvents(
  onProgress?: (p: LoadProgress) => void,
): Promise<TraceEvent[]> {
  const r = await fetch("/trace/events.jsonl");
  if (!r.ok) throw new Error(`events: HTTP ${r.status}`);
  const reader = r.body?.getReader();
  if (!reader) {
    const text = await r.text();
    return parseLines(text);
  }
  const dec = new TextDecoder();
  let buf = "";
  let bytes = 0;
  const events: TraceEvent[] = [];
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    bytes += value.length;
    buf += dec.decode(value, { stream: true });
    let nl: number;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (line) {
        try {
          events.push(JSON.parse(line) as TraceEvent);
        } catch {
          // Skip a torn line rather than failing a 50 MB load.
        }
      }
    }
    onProgress?.({ bytes, events: events.length, done: false });
  }
  const tail = (buf + dec.decode()).trim();
  if (tail) {
    try {
      events.push(JSON.parse(tail) as TraceEvent);
    } catch {
      // ignore
    }
  }
  onProgress?.({ bytes, events: events.length, done: true });
  return events;
}

function parseLines(text: string): TraceEvent[] {
  const out: TraceEvent[] = [];
  for (const line of text.split("\n")) {
    const t = line.trim();
    if (!t) continue;
    try {
      out.push(JSON.parse(t) as TraceEvent);
    } catch {
      // ignore
    }
  }
  return out;
}

export async function loadText(rel: string): Promise<string> {
  const r = await fetch(`/trace/${rel}`);
  if (!r.ok) throw new Error(`${rel}: HTTP ${r.status}`);
  return r.text();
}

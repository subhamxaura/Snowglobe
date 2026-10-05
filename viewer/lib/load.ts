// Trace loading over the view server's /api (paged, seq-indexed):
// manifest, then events in ≤5000/page slices, then blobs on demand per
// turn. Falls back to the early-Phase-1C /trace/* paths when talking to an
// older binary. No full-file JSON.parse: each page parses small.
import { MAX_PAGE } from "./model";
import type { LinksDoc } from "./model";
import type { Manifest, TraceEvent } from "./types";

export interface LoadProgress {
  bytes: number;
  events: number;
  done: boolean;
}

interface EventsPage {
  from: number;
  to: number;
  total: number;
  events: TraceEvent[];
}

async function getJson(path: string): Promise<{ status: number; body: string }> {
  const r = await fetch(path);
  return { status: r.status, body: r.status === 304 ? "" : await r.text() };
}

export async function loadManifest(): Promise<Manifest> {
  const api = await getJson("/api/manifest");
  if (api.status === 200) return JSON.parse(api.body) as Manifest;
  // Compat: pre-/api binaries served the raw file.
  const r = await fetch("/trace/manifest.json");
  if (!r.ok) throw new Error(`manifest: HTTP ${r.status}`);
  return (await r.json()) as Manifest;
}

async function loadPage(from: number, to: number): Promise<EventsPage | null> {
  const { status, body } = await getJson(`/api/events?from=${from}&to=${to}`);
  if (status !== 200) return null;
  return JSON.parse(body) as EventsPage;
}

// Streams pages (progress per page so the status line stays live on
// 50 MB traces). Returns all events; tolerates a torn line per page.
export async function loadEvents(
  onProgress?: (p: LoadProgress) => void,
): Promise<TraceEvent[]> {
  // Full pages (5000): a 50 MB trace loads in ~70 requests instead of
  // ~350 — same paging discipline, far less round-trip overhead.
  const step = MAX_PAGE;
  let from = 0;
  const events: TraceEvent[] = [];
  let bytes = 0;
  for (;;) {
    const page = await loadPage(from, from + step);
    if (!page) break; // old binary: fall through to legacy below
    bytes += JSON.stringify(page.events).length;
    events.push(...page.events);
    onProgress?.({ bytes, events: events.length, done: false });
    if (page.events.length === 0 || events.length >= page.total) break;
    from = events.length;
    if (from >= page.total) break;
  }
  if (events.length > 0) {
    onProgress?.({ bytes, events: events.length, done: true });
    return events;
  }
  return loadEventsLegacy(onProgress);
}

// Compat: pre-/api binaries served the whole file at /trace/events.jsonl.
async function loadEventsLegacy(
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
  const clean = rel.replace(/^\/+/, "");
  const api = await fetch(`/api/blob/${clean}`);
  if (api.ok) return api.text();
  // Compat: pre-/api binaries served blobs at /trace/<rel>.
  const r = await fetch(`/trace/${clean}`);
  if (!r.ok) throw new Error(`${rel}: HTTP ${r.status}`);
  return r.text();
}

// Causal sidecar (core/link, ADR-0006): null when the run has no
// links.json (404) or it is unreadable — the viewer then falls back to
// the seq heuristic and says so. Non-v1 versions also fall back.
export async function loadLinks(): Promise<LinksDoc | null> {
  const r = await fetch("/api/links");
  if (!r.ok) return null;
  try {
    const d = (await r.json()) as LinksDoc;
    if (d && d.version === 1 && Array.isArray(d.turns)) return d;
    return null;
  } catch {
    return null;
  }
}

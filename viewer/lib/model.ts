// Schema-0 typed event model + causal (turn) layer.
//
// Everything here is format-agnostic transport shape: no provider parsing,
// no pricing (those live in lib/providers/ + pricing.json). Unknown
// fields are ignored by readers — every accessor below tolerates their
// absence (forward compatibility, AGENTS.md §3).
//
// Turn rule (baked in from real runs):
//   - Every non-probe llm.response is a turn, INCLUDING non-2xx errors
//     (a no-credit run once reported "0 LLM turns" for two error
//     responses — never again).
//   - Probe requests (HEAD pre-flights, or model-less requests with no
//     body) are NOT turns and never split a span.
//   - A turn spans [its request seq, next turn's request seq): side
//     effects between response N and request N+1 belong to turn N.
//     (Heuristic only: links.json sidecar spans [res_N, res_N+1) in
//     res-completion order via a res-sorted index — core/link — so
//     out-of-order completions differ by design; sidecar wins where it
//     speaks, see TurnIndex below and docs/limitations.md.)
//   - Linking is 1D by seq for now (per-process-tree linking replaces the
//     inside later); the stable interface is getTurnForEvent(seq).
import type { Manifest, TraceEvent } from "./types";

export type { Manifest, TraceEvent };

export const MAX_PAGE = 5000;
export const DEFAULT_PAGE = 1000;

// ---- per-kind shapes (all fields optional: readers ignore unknowns) ----

export interface LlmRequest {
  id: number;
  provider: string;
  method: string;
  path: string;
  model: string | null;
  bytes: number;
  stream: boolean;
  seq: number;
}

export interface LlmResponse {
  id: number;
  status: number;
  bytes: number;
  ttfbMs: number | null;
  totalMs: number;
  chunks: number;
  truncated: boolean;
  reqRel: string;
  resRel: string;
  idxRel: string;
  seq: number;
}

function num(v: unknown): number | null {
  return typeof v === "number" && Number.isFinite(v) ? v : null;
}

function str(v: unknown): string | null {
  return typeof v === "string" ? v : null;
}

export function asRequest(e: TraceEvent): LlmRequest | null {
  if (e.ev !== "llm.request") return null;
  const id = num(e["id"]);
  if (id === null) return null;
  return {
    id,
    provider: str(e["provider"]) ?? "unknown",
    method: str(e["method"]) ?? "",
    path: str(e["path"]) ?? "",
    model: str(e["model"]),
    bytes: num(e["bytes"]) ?? 0,
    stream: e["stream"] === true,
    seq: seqOf(e),
  };
}

export function asResponse(e: TraceEvent): LlmResponse | null {
  if (e.ev !== "llm.response") return null;
  const id = num(e["id"]);
  const status = num(e["status"]);
  if (id === null || status === null) return null;
  return {
    id,
    status,
    bytes: num(e["bytes"]) ?? 0,
    ttfbMs: num(e["ttfb_ms"]),
    totalMs: num(e["total_ms"]) ?? 0,
    chunks: num(e["chunk_count"]) ?? 0,
    truncated: e["truncated"] === true,
    reqRel: str(e["req"]) ?? "",
    resRel: str(e["res"]) ?? "",
    idxRel: str(e["idx"]) ?? "",
    seq: seqOf(e),
  };
}

export function isErrorStatus(status: number): boolean {
  return status < 200 || status >= 300;
}

// Probe = HEAD pre-flight, or a model-less request with no body (SDK
// health checks). A model-less POST *with* a body (Gemini generateContent
// carries the model in the URL path, not the JSON) is a real turn.
// Mirrors the /api/summary rule in core/view/view.cpp — keep in sync.
export function isProbeRequest(r: LlmRequest): boolean {
  if (r.method === "HEAD") return true;
  return r.model === null && r.bytes === 0;
}

// ---- sequence numbers (normalised fixtures strip seq: fall back to order) ----

export function seqOf(e: TraceEvent, fallback = 0): number {
  const s = num(e["seq"]);
  return s === null ? fallback : s;
}

// ---- network endpoints (structured fields + legacy addr fallback) ----

export type NetBucket = "tcp" | "unix" | "other";

export interface Endpoint {
  family: string;
  label: string;
  bucket: NetBucket;
  host?: string;
  port?: number;
  path?: string;
}

// Handles net.disconnect (no peer: the AF_UNSPEC UDP-disconnect idiom) and
// net.connect/sendto/bind structured family/ip/port/path fields, with the
// legacy formatted addr string as fallback. Never throws on agent traffic.
export function endpointOf(e: TraceEvent): Endpoint | null {
  if (e.ev === "net.disconnect") {
    return { family: "unspec", label: "(disconnect)", bucket: "other" };
  }
  if (e.ev !== "net.connect" && e.ev !== "net.sendto" && e.ev !== "net.bind") {
    return null;
  }
  const family = str(e["family"]) ?? (e["addr"] !== undefined ? "unknown" : "unknown");
  if (family === "unix") {
    const p = str(e["path"]) ?? str(e["addr"]) ?? "";
    return { family, label: `unix:${p}`, bucket: "unix", path: p };
  }
  if (family === "ipv4" || family === "ipv6") {
    const ip = str(e["ip"]) ?? "";
    const port = num(e["port"]) ?? undefined;
    const host = port === undefined ? ip : `${ip}:${port}`;
    return { family, label: host || String(e["addr"] ?? family), bucket: "tcp", host: ip, port };
  }
  const addr = str(e["addr"]);
  if (addr !== null) {
    if (addr.startsWith("unix:")) {
      return { family, label: addr, bucket: "unix", path: addr.slice(5) };
    }
    return { family, label: addr, bucket: "tcp", host: addr };
  }
  return { family, label: family, bucket: "other" };
}

export function hostKey(e: TraceEvent): string | null {
  const ep = endpointOf(e);
  return ep ? `${ep.bucket}:${ep.label}` : null;
}

// ---- file events ----

export function isWriteOpen(e: TraceEvent): boolean {
  return e.ev === "fs.open" && e["write"] === true && typeof e["path"] === "string";
}

export function isDeleteEvent(e: TraceEvent): boolean {
  return e.ev === "fs.unlink" || e.ev === "fs.rmdir";
}

// ---- turns ----

export interface Turn {
  id: number; // llm id
  provider: string;
  model: string | null;
  stream: boolean;
  status: number;
  error: boolean;
  reqSeq: number;
  resSeq: number;
  reqRel: string;
  resRel: string;
}

// Build turns from a flat event list. Order-agnostic (sorts by seq);
// events without seq use array order (normalised fixtures).
export function buildTurns(events: TraceEvent[]): Turn[] {
  const reqs = new Map<number, LlmRequest>();
  const resps: LlmResponse[] = [];
  events.forEach((e, i) => {
    const r = asRequest({ ...e, seq: seqOf(e, i) });
    if (r) {
      reqs.set(r.id, r);
      return;
    }
    const s = asResponse({ ...e, seq: seqOf(e, i) });
    if (s) resps.push(s);
  });
  resps.sort((a, b) => a.seq - b.seq);
  const turns: Turn[] = [];
  for (const s of resps) {
    const q = reqs.get(s.id);
    if (q && isProbeRequest(q)) continue; // probes are never turns
    turns.push({
      id: s.id,
      provider: q?.provider ?? "unknown",
      model: q?.model ?? null,
      stream: q?.stream ?? false,
      status: s.status,
      error: isErrorStatus(s.status),
      reqSeq: q?.seq ?? s.seq,
      resSeq: s.seq,
      reqRel: s.reqRel,
      resRel: s.resRel,
    });
  }
  turns.sort((a, b) => a.reqSeq - b.reqSeq);
  return turns;
}

// Stable causal interface: which turn owns event seq? Binary search over
// turn spans; null before the first turn. The 1D linker replaces the
// inside later — this signature stays.
export interface LinksAttrib {
  seq: number;
  basis: string;
  confidence: string;
}

export interface LinksUnattr {
  seq: number;
  reason: string;
}

export interface LinksTurn {
  turn: number; // llm id (mirrors Turn.id)
  llm: { req: number; res: number; tools: string[] };
  attributed: LinksAttrib[];
  unattributed: LinksUnattr[];
}

// links.json as served by /api/links (core/link, ADR-0006). Unknown
// basis/reason/confidence values must be tolerated (forward compat).
export interface LinksDoc {
  version: number;
  turns: LinksTurn[];
}

export type LinkSource = "sidecar" | "heuristic";

export interface LinkHit {
  turn: Turn;
  basis: string;
  confidence: string;
}

export class TurnIndex {
  private turns: Turn[];
  private exact = new Map<number, { turn: number; basis: string; confidence: string }>();
  private reasons = new Map<number, string>(); // seq → unattributed reason
  readonly source: LinkSource;

  constructor(events: TraceEvent[], links?: LinksDoc | null) {
    this.turns = buildTurns(events);
    if (links && links.version === 1) {
      for (const lt of links.turns) {
        for (const a of lt.attributed) {
          this.exact.set(a.seq, {
            turn: lt.turn,
            basis: typeof a.basis === "string" ? a.basis : "window",
            confidence: typeof a.confidence === "string" ? a.confidence : "high",
          });
        }
        for (const u of lt.unattributed) {
          if (typeof u.reason === "string") this.reasons.set(u.seq, u.reason);
        }
      }
      this.source = "sidecar";
    } else {
      this.source = "heuristic";
    }
  }

  get list(): Turn[] {
    return this.turns;
  }

  // Sidecar hit wins (response-anchored links.json is truth where it
  // speaks); otherwise the request-anchored span heuristic. The two
  // anchorings differ by design (docs/limitations.md) — unlisted seqs
  // keep the old answer, never null-by-surprise.
  getTurnForEvent(seq: number): Turn | null {
    const hit = this.exact.get(seq);
    if (hit !== undefined) {
      const turn = this.turns.find((t) => t.id === hit.turn) ?? null;
      if (turn) return turn;
      // Stale sidecar (turn id with no Turn here): fall through.
    }
    let lo = 0;
    let hi = this.turns.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.turns[mid].reqSeq <= seq) lo = mid + 1;
      else hi = mid;
    }
    return lo === 0 ? null : this.turns[lo - 1];
  }

  // Evidence for one seq: linked (turn + basis + confidence), explicitly
  // unattributed (reason), or null (heuristic territory).
  linkOf(seq: number): LinkHit | { unattributed: string } | null {
    const hit = this.exact.get(seq);
    if (hit !== undefined) {
      const turn = this.turns.find((t) => t.id === hit.turn) ?? null;
      if (!turn) return null;
      return { turn, basis: hit.basis, confidence: hit.confidence };
    }
    const reason = this.reasons.get(seq);
    if (reason !== undefined) return { unattributed: reason };
    return null;
  }
}

// ---- /api/summary mirror ----

export interface TraceSummary {
  events: number;
  turns: number;
  error_turns: number;
  probe_requests: number;
  processes: number;
  duration_ms: number;
  kinds: Record<string, number>;
  hosts: { tcp: Record<string, number>; unix: Record<string, number>; disconnects: number };
  files: { written: number; deleted: number; renamed: number };
}

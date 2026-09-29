// Shared trace types (schema v0). Unknown fields are ignored by readers;
// these interfaces cover what the views need, nothing more.
export interface TraceEvent {
  seq: number;
  ts_us: number;
  t_ms: number;
  ev: string;
  pid: number | string;
  tid: number | string;
  [k: string]: unknown;
}

export interface Manifest {
  schema: number;
  snowglobe_version: string;
  started: string;
  finished: string | null;
  cmd: string[];
  cwd: string;
  event_count: number;
  last_hash: string;
  [k: string]: unknown;
}

export interface LlmTurn {
  id: number;
  provider: string;
  model: string | null;
  stream: boolean;
  status: number;
  bytes: number;
  ttfb_ms: number | null;
  total_ms: number;
  chunks: number;
  truncated: boolean;
  reqRel: string;
  resRel: string;
}

export interface ToolCall {
  id: string;
  name: string;
  args: string;
}

export interface ParsedMessage {
  text: string;
  toolCalls: ToolCall[];
  usage: { input: number; output: number } | null;
  costUsd: number | null;
}

export interface ProcNode {
  pid: string;
  ppid: string;
  thread: boolean;
  root: boolean;
  execs: TraceEvent[];
  exit: TraceEvent | null;
  children: ProcNode[];
}

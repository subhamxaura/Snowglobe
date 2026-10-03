// Shared provider-folding shapes. Parsers fold provider payloads into
// Folded and never throw on agent traffic — unknown shapes stay empty.
export interface FoldedUsage {
  input: number;
  output: number;
}

export interface Folded {
  text: string;
  toolCalls: { id: string; name: string; args: string }[];
  usage: FoldedUsage | null;
}

// Parsed provider error envelope (Anthropic {"type":"error",...} or
// OpenAI {"error":{...}}). null = no recognisable envelope.
export interface ErrorInfo {
  type: string;
  message: string;
}

export function safeJson(s: string): unknown {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

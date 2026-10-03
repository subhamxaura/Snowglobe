// Provider parsing lives HERE (TypeScript), never in C++ (locked).
// Public surface: the per-provider folders live under providers/
// (openai.ts, anthropic.ts) and fold full bodies and SSE streams into
// {text, toolCalls, usage}; pricing.ts owns the static cost table.
// Unknown shapes yield empty text and null usage/cost — never throw on
// agent traffic.
export type { ErrorInfo, Folded, FoldedUsage } from "./providers/types";
export { parseSseFrames } from "./providers/sse";
export { costUsd, priceVerified } from "./providers/pricing";
export { foldOpenAi, openaiErrorInfo } from "./providers/openai";
export { foldAnthropic, anthropicErrorInfo } from "./providers/anthropic";

import { anthropicErrorInfo } from "./providers/anthropic";
import { foldAnthropic } from "./providers/anthropic";
import { openaiErrorInfo } from "./providers/openai";
import { foldOpenAi } from "./providers/openai";
import type { ErrorInfo, Folded } from "./providers/types";

export function foldBody(
  provider: string,
  bodyText: string,
  streamed: boolean,
): Folded {
  if (provider === "anthropic") return foldAnthropic(bodyText, streamed);
  return foldOpenAi(bodyText, streamed);
}

// Best-effort provider error envelope for a response body. Error turns
// render type + message; unrecognised bodies yield null (never throw).
export function errorInfo(provider: string, bodyText: string): ErrorInfo | null {
  if (provider === "anthropic") return anthropicErrorInfo(bodyText);
  return openaiErrorInfo(bodyText);
}

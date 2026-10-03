// Static $/1M-token table from lib/pricing.json — every entry carries the
// date its price was verified against the provider's official page, and
// each price value is a positive number (n/a is absence, never 0; the
// test suite asserts the shape). Unknown models cost null, never a
// guessed number. The mock LLM's "mock-model-1" has no entry on purpose:
// viewer-wide, synthetic costs would be a lie.
import pricing from "../pricing.json";
import type { FoldedUsage } from "./types";

const PRICE = pricing.models as Record<
  string,
  { input: number; output: number; verified: string }
>;

export function costUsd(
  model: string | null,
  usage: FoldedUsage | null,
): number | null {
  if (!model || !usage) return null;
  // Own-property lookup: a hostile trace's model id must not resolve to
  // Object.prototype members ("constructor", "toString") and render $NaN.
  const p = Object.prototype.hasOwnProperty.call(PRICE, model)
    ? PRICE[model]
    : undefined;
  if (!p) return null;
  return (usage.input * p.input + usage.output * p.output) / 1e6;
}

export function priceVerified(model: string): string | null {
  return PRICE[model]?.verified ?? null;
}

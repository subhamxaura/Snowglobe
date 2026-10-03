// Split a raw SSE stream into data frames (comments/blank lines dropped,
// "[DONE]" kept as a terminal marker the folders ignore).
export interface SseFrame {
  data: string[];
}

export function parseSseFrames(text: string): SseFrame[] {
  const frames: SseFrame[] = [];
  for (const block of text.split(/\r?\n\r?\n/)) {
    const data: string[] = [];
    for (const line of block.split(/\r?\n/)) {
      if (line.startsWith("data:")) data.push(line.slice(5).trimStart());
    }
    if (data.length > 0) frames.push({ data });
  }
  return frames;
}

import { useMemo, useRef, useState } from "react";

// Windowed list: renders ~40 rows around the scroll position so a 500k-row
// trace scrolls without dropping frames. Rows have fixed height.
export default function VList<T>({
  items,
  rowHeight,
  height,
  renderRow,
  keyOf,
}: {
  items: readonly T[];
  rowHeight: number;
  height: number;
  renderRow: (item: T, index: number) => React.ReactNode;
  keyOf: (item: T, index: number) => string | number;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const [top, setTop] = useState(0);
  const total = items.length * rowHeight;
  const start = Math.max(0, Math.floor(top / rowHeight) - 5);
  const count = Math.ceil(height / rowHeight) + 10;
  const slice = useMemo(
    () => items.slice(start, start + count),
    [items, start, count],
  );
  return (
    <div
      ref={ref}
      className="vlist"
      data-testid="vlist"
      style={{ height }}
      onScroll={(e) => setTop((e.target as HTMLDivElement).scrollTop)}
    >
      <div style={{ height: total, position: "relative" }}>
        {slice.map((item, k) => {
          const i = start + k;
          return (
            <div
              key={keyOf(item, i)}
              data-seq={i}
              style={{
                position: "absolute",
                top: i * rowHeight,
                height: rowHeight,
                left: 0,
                right: 0,
                overflow: "hidden",
              }}
            >
              {renderRow(item, i)}
            </div>
          );
        })}
      </div>
    </div>
  );
}

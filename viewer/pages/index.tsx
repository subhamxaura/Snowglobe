import { useEffect, useState } from "react";
import Timeline from "../components/Timeline";
import Turns from "../components/Turns";
import Processes from "../components/Processes";
import Files from "../components/Files";
import Network from "../components/Network";
import Search from "../components/Search";
import { loadEvents, loadManifest, loadReport, replayBadge } from "../lib/load";
import type { Manifest, ReplayReport, TraceEvent } from "../lib/types";

const TABS = ["turns", "timeline", "processes", "files", "network", "search"] as const;

export default function Home() {
  const [manifest, setManifest] = useState<Manifest | null>(null);
  const [report, setReport] = useState<ReplayReport | null>(null);
  const [events, setEvents] = useState<TraceEvent[]>([]);
  const [status, setStatus] = useState("loading manifest…");
  const [tab, setTab] = useState<(typeof TABS)[number]>("turns");
  const [error, setError] = useState("");

  useEffect(() => {
    (async () => {
      try {
        const m = await loadManifest();
        setManifest(m);
        // Replay verdict is best-effort (404 on live runs): never blocks.
        loadReport().then(setReport, () => {});
        setStatus("loading events…");
        const evs = await loadEvents((p) => {
          if (!p.done) setStatus(`loading… ${p.events} events`);
        });
        setEvents(evs);
        setStatus("");
      } catch (e) {
        setError(`cannot load trace (is a run mounted at /trace?): ${e}`);
        setStatus("");
      }
    })();
  }, []);

  return (
    <main>
      <h1>snowglobe</h1>
      {manifest && (
        <p className="muted" data-testid="manifest">
          {manifest.cmd.join(" ")} · {manifest.event_count} events ·{" "}
          {manifest.snowglobe_version}
        </p>
      )}
      {manifest && replayBadge(manifest, report) && (
        <p data-testid="replay-badge">{replayBadge(manifest, report)}</p>
      )}
      {status && <p data-testid="status">{status}</p>}
      {error && <p data-testid="error">{error}</p>}
      {events.length > 0 && (
        <>
          <div className="tabs" role="tablist">
            {TABS.map((t) => (
              <button
                key={t}
                role="tab"
                aria-selected={tab === t}
                onClick={() => setTab(t)}
              >
                {t}
              </button>
            ))}
          </div>
          <div data-testid={`view-${tab}`}>
            {tab === "turns" && <Turns events={events} />}
            {tab === "timeline" && <Timeline events={events} />}
            {tab === "processes" && <Processes events={events} />}
            {tab === "files" && <Files events={events} />}
            {tab === "network" && <Network events={events} />}
            {tab === "search" && <Search events={events} />}
          </div>
        </>
      )}
    </main>
  );
}

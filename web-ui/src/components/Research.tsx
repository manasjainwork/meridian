import { useEffect, useState } from "react";

interface Milestone { group: string; name: string; state: "done" | "partial" | "todo"; note: string; }
interface Status {
  generated_at: string;
  milestones: Milestone[];
  doubts: string[];
  segments?: { count: number; hours: number; median_s: number; max_s: number; ticks: number; trades: number; from: string; to: string; buckets: { label: string; count: number }[] };
  checksums?: { ok: number; bad: number; files: number };
  stats?: { k: string; v: string }[];
  recorder?: { files_gz: number; last_log_line: string | null };
}

const n = (x: number) => x.toLocaleString("en-US");

function Ms({ items }: { items: Milestone[] }) {
  return (
    <>
      {items.map((m) => (
        <div className="part" key={m.name}>
          <span className={"st " + m.state}>{m.state}</span>
          <div><b>{m.name}</b>{m.note && <div className="small muted">{m.note}</div>}</div>
        </div>
      ))}
    </>
  );
}

// Reads status.json, written by tools/ops/status.py from the recordings on one laptop. A snapshot, not a live view.
export default function Research() {
  const [d, setD] = useState<Status | null>(null);
  const [err, setErr] = useState("");
  useEffect(() => {
    fetch("status.json", { cache: "no-store" })
      .then((r) => { if (!r.ok) throw new Error(`status.json: HTTP ${r.status}`); return r.json(); })
      .then(setD).catch((e) => setErr(String(e)));
  }, []);
  if (err) return <p className="err">Could not load the snapshot: {err}</p>;
  if (!d) return <p className="muted">Loading...</p>;
  const s = d.segments;
  const max = s ? Math.max(1, ...s.buckets.map((b) => b.count)) : 1;
  const tiles: [string, string][] = [];
  if (s) {
    tiles.push(["Recorded", `${s.hours} h in ${n(s.count)} segments`], ["Book messages", n(s.ticks)], ["Trades", n(s.trades)],
      ["Median / longest segment", `${s.median_s} s / ${s.max_s} s`], ["Period", `${s.from.slice(0, 10)} to ${s.to.slice(0, 10)}`]);
  }
  if (d.checksums) tiles.push(["Kraken CRC32 matches", n(d.checksums.ok)], ["Mismatches", `${n(d.checksums.bad)} (${d.checksums.files} files)`]);
  return (
    <div className="arch">
      <div className="panel">
        <h3>Research snapshot, {d.generated_at}</h3>
        <div className="small muted">
          Not live. Numbers come from files on one laptop and describe those recordings only. WSL2 numbers are observations, not benchmarks.
          No agent has been trained and there are no research results yet.
        </div>
      </div>
      {tiles.length > 0 && (
        <div className="stats">
          {tiles.map(([k, v]) => <div className="tile" key={k}><div className="k">{k}</div><div className="v">{v}</div></div>)}
        </div>
      )}
      <div className="rgrid">
        {s && (
          <div className="panel">
            <h3>Segment lengths</h3>
            {s.buckets.map((b) => (
              <div className="hrow" key={b.label}>
                <span>{b.label}</span>
                <div className="hbar"><i style={{ width: `${(100 * b.count) / max}%` }} /></div>
                <span>{b.count}</span>
              </div>
            ))}
            <div className="small muted">A new segment starts at every reconnect. Segments are never joined or filled.</div>
          </div>
        )}
        {d.stats && (
          <div className="panel">
            <h3>Recorded-market stats</h3>
            <table className="kv"><tbody>{d.stats.map((r) => <tr key={r.k}><td>{r.k}</td><td>{r.v}</td></tr>)}</tbody></table>
          </div>
        )}
      </div>
      <div className="rgrid">
        <div className="panel"><h3>Engine milestones</h3><Ms items={d.milestones.filter((m) => m.group === "engine")} /></div>
        <div className="panel"><h3>Data and ML</h3><Ms items={d.milestones.filter((m) => m.group === "ml")} /></div>
      </div>
      <div className="panel">
        <h3>Not established yet</h3>
        <ul className="small">{d.doubts.map((x) => <li key={x}>{x}</li>)}</ul>
        {d.recorder && <div className="small muted">Recorder: {d.recorder.files_gz} finished files. Last log line: {d.recorder.last_log_line ?? "none"}</div>}
      </div>
    </div>
  );
}

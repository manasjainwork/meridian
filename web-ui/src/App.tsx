import { useEffect, useState } from "react";
import type { Replay } from "./types";
import { useReplay } from "./useReplay";
import Ladder from "./components/Ladder";
import DepthChart from "./components/DepthChart";
import PriceChart from "./components/PriceChart";
import Architecture from "./components/Architecture";
import Trade from "./components/Trade";
import Controls from "./components/Controls";
import Stats from "./components/Stats";
import Tape from "./components/Tape";

export default function App() {
  const [replay, setReplay] = useState<Replay | null>(null);
  const [err, setErr] = useState("");
  const [tab, setTab] = useState<"market" | "trade" | "arch">("market");
  useEffect(() => {
    fetch("replay.json").then((r) => { if (!r.ok) throw new Error(`replay.json: HTTP ${r.status}`); return r.json(); })
      .then(setReplay).catch((e) => setErr(String(e)));
  }, []);
  const rp = useReplay(replay);

  if (err) return <div className="page"><p className="err">Could not load the recording: {err}</p></div>;
  if (!replay) return <div className="page"><p className="muted">Loading recording...</p></div>;

  const f = replay.frames[rp.idx];
  const played = (replay.trades ?? []).filter((t) => t.t <= f.t);
  return (
    <div className="page">
      <header>
        <h1>Meridian</h1>
        <span className="muted">C++20 matching engine, replicated with Raft</span>
        <span className="badge">
          REPLAY of recorded {replay.symbol} market data ({replay.source}), not live, not a benchmark
          {replay.gaps_capped_ms ? `, silences over ${replay.gaps_capped_ms} ms shortened` : ""}{replay.sampled_ms ? `, one frame per ${replay.sampled_ms} ms` : ""}
        </span>
      </header>
      <div className="seg tabs">
        <button className={tab === "market" ? "on" : ""} onClick={() => setTab("market")}>Market</button>
        <button className={tab === "trade" ? "on" : ""} onClick={() => setTab("trade")}>Trade (needs gateway)</button>
        <button className={tab === "arch" ? "on" : ""} onClick={() => setTab("arch")}>Architecture</button>
      </div>
      {tab === "arch" ? <Architecture /> : tab === "trade" ? <Trade /> : <>
      <Controls playing={rp.playing} speed={rp.speed} idx={rp.idx} count={replay.frames.length} seconds={f.t / 1000}
        onPlay={rp.setPlaying} onSpeed={rp.setSpeed} onSeek={rp.seek} />
      <Stats frame={f} trades={played} />
      <div className="grid">
        <div className="stack">
          <Ladder frame={f} />
          <Tape trades={played} />
        </div>
        <div className="stack">
          <PriceChart frames={replay.frames} idx={rp.idx} trades={played} />
          <DepthChart frame={f} />
        </div>
      </div>
      </>}
    </div>
  );
}

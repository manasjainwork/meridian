import { useState } from "react";
import type { Frame, Trade } from "../types";
import MidChart from "./MidChart";
import Candles from "./Candles";

type Mode = "line" | "c1" | "c5";

export default function PriceChart({ frames, idx, trades }: { frames: Frame[]; idx: number; trades: Trade[] }) {
  const [mode, setMode] = useState<Mode>("line");
  const tabs: [Mode, string][] = [["line", "Line"], ["c1", "Candles 1 s"], ["c5", "Candles 5 s"]];
  return (
    <div>
      <div className="seg">{tabs.map(([m, label]) => <button key={m} className={mode === m ? "on" : ""} onClick={() => setMode(m)}>{label}</button>)}</div>
      {mode === "line" ? <MidChart frames={frames} idx={idx} trades={trades} /> : (
        <div className="panel"><h3>Mid price candles ({mode === "c1" ? "1 s" : "5 s"})</h3>
          <Candles frames={frames} idx={idx} trades={trades} secs={mode === "c1" ? 1 : 5} /></div>
      )}
    </div>
  );
}

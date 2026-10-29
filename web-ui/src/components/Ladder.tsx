import { useRef } from "react";
import type { Frame } from "../types";

const ROWS = 10;
const fmt = (p: number) => p.toLocaleString("en-US", { minimumFractionDigits: 1, maximumFractionDigits: 1 });

// Fixed number of rows and a slowly decaying bar scale, so the bars do not jump around.
export default function Ladder({ frame }: { frame: Frame }) {
  const scale = useRef(0.01);
  const asks = frame.a.slice(0, ROWS).reverse();
  const bids = frame.b.slice(0, ROWS);
  const peak = Math.max(0.0001, ...asks.map((l) => l[1]), ...bids.map((l) => l[1]));
  scale.current = Math.max(peak, scale.current * 0.995);
  const bar = (q: number) => `${Math.min(100, (q / scale.current) * 100)}%`;
  const spread = frame.a[0][0] - frame.b[0][0];
  return (
    <div className="panel">
      <h3>Order book</h3>
      <div className="ladder">
        <div className="row head"><span>Price</span><span>Size (BTC)</span></div>
        {asks.map((l) => (
          <div className="row ask" key={"a" + l[0]}>
            <i style={{ width: bar(l[1]) }} /><span>{fmt(l[0])}</span><span>{l[1].toFixed(4)}</span>
          </div>
        ))}
        <div className="row mid"><span>spread {spread.toFixed(1)}</span><span>mid {fmt((frame.a[0][0] + frame.b[0][0]) / 2)}</span></div>
        {bids.map((l) => (
          <div className="row bid" key={"b" + l[0]}>
            <i style={{ width: bar(l[1]) }} /><span>{fmt(l[0])}</span><span>{l[1].toFixed(4)}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

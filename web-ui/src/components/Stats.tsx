import type { Frame, Trade } from "../types";

// Numbers derived from the book on screen and the trades played so far. Nothing here is an engine measurement.
export default function Stats({ frame, trades }: { frame: Frame; trades: Trade[] }) {
  const bb = frame.b[0], ba = frame.a[0];
  const mid = (bb[0] + ba[0]) / 2;
  const spreadBps = ((ba[0] - bb[0]) / mid) * 1e4;
  const bq = frame.b.slice(0, 5).reduce((s, l) => s + l[1], 0), aq = frame.a.slice(0, 5).reduce((s, l) => s + l[1], 0);
  const imb = (bq - aq) / (bq + aq || 1);
  const vol = trades.reduce((s, t) => s + t.q, 0);
  const buy = trades.filter((t) => t.s === "buy").reduce((s, t) => s + t.q, 0);
  const items: [string, string][] = [
    ["Mid", mid.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })],
    ["Spread", `${(ba[0] - bb[0]).toFixed(1)}  (${spreadBps.toFixed(2)} bps)`],
    ["Best bid / ask size", `${bb[1].toFixed(4)} / ${ba[1].toFixed(4)}`],
    ["Imbalance (top 5)", `${imb >= 0 ? "+" : ""}${(imb * 100).toFixed(0)}% ${imb >= 0 ? "bids" : "asks"}`],
    ["Trades so far", String(trades.length)],
    ["Volume / buy share", trades.length ? `${vol.toFixed(4)} BTC / ${((buy / (vol || 1)) * 100).toFixed(0)}%` : "n/a"],
  ];
  return (
    <div className="stats">
      {items.map(([k, v]) => (
        <div className="tile" key={k}><div className="k">{k}</div><div className="v">{v}</div></div>
      ))}
    </div>
  );
}

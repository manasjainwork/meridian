import type { Trade } from "../types";

// Most recent trades first. Buy = the aggressor bought (lifted the ask), sell = the aggressor sold (hit the bid).
export default function Tape({ trades }: { trades: Trade[] }) {
  const rows = trades.slice(-14).reverse();
  return (
    <div className="panel">
      <h3>Trades</h3>
      {rows.length === 0 && <p className="muted small">No trades in this recording yet.</p>}
      <div className="tape">
        {rows.map((t, i) => (
          <div className={"trow " + t.s} key={t.t + "-" + i}>
            <span>{t.p.toLocaleString("en-US", { minimumFractionDigits: 1, maximumFractionDigits: 1 })}</span>
            <span>{t.q.toFixed(4)}</span>
            <span className="muted">{(t.t / 1000).toFixed(1)} s</span>
          </div>
        ))}
      </div>
    </div>
  );
}

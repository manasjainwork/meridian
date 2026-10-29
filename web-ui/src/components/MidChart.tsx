import { useState } from "react";
import type { Frame, Trade } from "../types";

const W = 520, H = 240, PX = 54, PY = 14, WIN = 30000; // show a rolling 30 s window

// Mid price over the last 30 s of the recording that has been played, trades as dots, hover shows time and price.
export default function MidChart({ frames, idx, trades }: { frames: Frame[]; idx: number; trades: Trade[] }) {
  const [hover, setHover] = useState<number | null>(null);
  const now = frames[idx].t;
  const start = Math.max(0, now - WIN), end = start + WIN;
  let first = idx;
  while (first > 0 && frames[first - 1].t >= start) first--;
  const step = Math.max(1, Math.ceil((idx - first + 1) / 300));
  const pts: [number, number][] = [];
  for (let i = first; i <= idx; i += step) pts.push([frames[i].t, (frames[i].a[0][0] + frames[i].b[0][0]) / 2]);
  if (pts[pts.length - 1][0] !== now) pts.push([now, (frames[idx].a[0][0] + frames[idx].b[0][0]) / 2]);
  const tr = trades.filter((t) => t.t >= start);
  let lo = Math.min(...pts.map((p) => p[1])), hi = Math.max(...pts.map((p) => p[1]));
  for (const t of tr) { lo = Math.min(lo, t.p); hi = Math.max(hi, t.p); }
  if (hi - lo < 1) { lo -= 0.5; hi += 0.5; }
  const x = (t: number) => PX + ((t - start) / WIN) * (W - PX - 8);
  const y = (v: number) => H - PY - ((v - lo) / (hi - lo)) * (H - 2 * PY);
  const d = pts.map((p, i) => `${i ? "L" : "M"} ${x(p[0])} ${y(p[1])}`).join(" ");
  const ticks = [0, 0.5, 1].map((f) => lo + f * (hi - lo));
  const onMove = (e: React.MouseEvent<SVGSVGElement>) => {
    const r = e.currentTarget.getBoundingClientRect();
    const px = ((e.clientX - r.left) / r.width) * W;
    const t = start + ((px - PX) / (W - PX - 8)) * WIN;
    let best = 0;
    for (let i = 0; i < pts.length; i++) if (Math.abs(pts[i][0] - t) < Math.abs(pts[best][0] - t)) best = i;
    setHover(best);
  };
  const h = hover !== null && hover < pts.length ? pts[hover] : null;
  return (
    <div className="panel">
      <h3>Mid price and trades (last 30 s)</h3>
      <svg viewBox={`0 0 ${W} ${H}`} className="chart" onMouseMove={onMove} onMouseLeave={() => setHover(null)}>
        {ticks.map((v) => (
          <g key={v}><line x1={PX} x2={W - 8} y1={y(v)} y2={y(v)} className="grid-line" /><text x={PX - 6} y={y(v) + 3} textAnchor="end" className="axis">{v.toFixed(1)}</text></g>
        ))}
        <text x={PX} y={H - 1} className="axis">{(start / 1000).toFixed(0)} s</text>
        <text x={W - 8} y={H - 1} textAnchor="end" className="axis">{(end / 1000).toFixed(0)} s</text>
        <path d={d} className="line" />
        {tr.map((t, i) => <circle key={i} cx={x(t.t)} cy={y(t.p)} r={2.6} className={t.s === "buy" ? "dot-buy" : "dot-sell"} />)}
        {h && <g><line x1={x(h[0])} x2={x(h[0])} y1={PY} y2={H - PY} className="cross" /><circle cx={x(h[0])} cy={y(h[1])} r={3.5} className="cross-dot" />
          <text x={Math.min(x(h[0]) + 6, W - 120)} y={PY + 10} className="axis hov">{(h[0] / 1000).toFixed(2)} s  mid {h[1].toFixed(2)}</text></g>}
      </svg>
    </div>
  );
}

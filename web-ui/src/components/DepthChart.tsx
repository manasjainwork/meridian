import type { Frame } from "../types";

const W = 520, H = 220, PAD = 8;

// Cumulative size against price: bids to the left of the mid, asks to the right.
export default function DepthChart({ frame }: { frame: Frame }) {
  const cum = (ls: [number, number][]) => { let s = 0; return ls.map(([p, q]) => [p, (s += q)] as [number, number]); };
  const b = cum(frame.b), a = cum(frame.a);
  const lo = b[b.length - 1][0], hi = a[a.length - 1][0];
  const maxQ = Math.max(b[b.length - 1][1], a[a.length - 1][1]);
  const x = (p: number) => PAD + ((p - lo) / (hi - lo || 1)) * (W - 2 * PAD);
  const y = (q: number) => H - PAD - (q / (maxQ || 1)) * (H - 2 * PAD);
  const path = (pts: [number, number][]) => {
    let d = `M ${x(pts[0][0])} ${y(0)}`;
    let prev = 0;
    for (const [p, q] of pts) { d += ` L ${x(p)} ${y(prev)} L ${x(p)} ${y(q)}`; prev = q; }
    return d + ` L ${x(pts[pts.length - 1][0])} ${y(0)} Z`;
  };
  return (
    <div className="panel">
      <h3>Depth (cumulative, 15 levels a side)</h3>
      <svg viewBox={`0 0 ${W} ${H}`} className="chart">
        <path d={path(b)} className="depth-bid" />
        <path d={path(a)} className="depth-ask" />
        <text x={PAD} y={H - 1} className="axis">{lo.toFixed(1)}</text>
        <text x={W - PAD} y={H - 1} className="axis" textAnchor="end">{hi.toFixed(1)}</text>
      </svg>
    </div>
  );
}

import type { Frame, Trade } from "../types";

const W = 520, H = 240, PX = 54, PY = 14, VOL = 40;

interface Candle { t: number; o: number; h: number; l: number; c: number; v: number; }

// Candles of the MID price (the book's middle, not trade prices) with traded volume under them.
export default function Candles({ frames, idx, trades, secs }: { frames: Frame[]; idx: number; trades: Trade[]; secs: number }) {
  const bucket = secs * 1000, count = 40;
  const now = frames[idx].t;
  const lastB = Math.floor(now / bucket), firstB = Math.max(0, lastB - count + 1);
  const cs = new Map<number, Candle>();
  let i = idx;
  while (i > 0 && frames[i - 1].t >= firstB * bucket) i--;
  for (; i <= idx; i++) {
    const b = Math.floor(frames[i].t / bucket); const m = (frames[i].a[0][0] + frames[i].b[0][0]) / 2;
    const c = cs.get(b);
    if (!c) cs.set(b, { t: b, o: m, h: m, l: m, c: m, v: 0 }); else { c.h = Math.max(c.h, m); c.l = Math.min(c.l, m); c.c = m; }
  }
  for (const t of trades) { const c = cs.get(Math.floor(t.t / bucket)); if (c) c.v += t.q; }
  const list = [...cs.values()].sort((a, b) => a.t - b.t);
  let lo = Math.min(...list.map((c) => c.l)), hi = Math.max(...list.map((c) => c.h));
  if (hi - lo < 1) { lo -= 0.5; hi += 0.5; }
  const vmax = Math.max(1e-9, ...list.map((c) => c.v));
  const cw = (W - PX - 8) / count;
  const x = (b: number) => PX + (b - firstB) * cw;
  const y = (v: number) => H - PY - VOL - ((v - lo) / (hi - lo)) * (H - 2 * PY - VOL);
  const ticks = [0, 0.5, 1].map((f) => lo + f * (hi - lo));
  return (
    <svg viewBox={`0 0 ${W} ${H}`} className="chart">
      {ticks.map((v) => (
        <g key={v}><line x1={PX} x2={W - 8} y1={y(v)} y2={y(v)} className="grid-line" /><text x={PX - 6} y={y(v) + 3} textAnchor="end" className="axis">{v.toFixed(1)}</text></g>
      ))}
      {list.map((c) => {
        const up = c.c >= c.o, cx = x(c.t) + cw / 2, top = y(Math.max(c.o, c.c)), bot = y(Math.min(c.o, c.c));
        return (
          <g key={c.t} className={up ? "c-up" : "c-down"}>
            <line x1={cx} x2={cx} y1={y(c.h)} y2={y(c.l)} />
            <rect x={x(c.t) + cw * 0.15} y={top} width={cw * 0.7} height={Math.max(1, bot - top)} />
            <rect x={x(c.t) + cw * 0.15} y={H - PY - (c.v / vmax) * VOL} width={cw * 0.7} height={(c.v / vmax) * VOL} className="vol" />
          </g>
        );
      })}
      <text x={PX} y={H - 1} className="axis">{(firstB * secs).toFixed(0)} s</text>
      <text x={W - 8} y={H - 1} textAnchor="end" className="axis">{((firstB + count) * secs).toFixed(0)} s</text>
      <text x={W - 8} y={PY - 2} textAnchor="end" className="axis">volume bars: traded BTC per candle</text>
    </svg>
  );
}

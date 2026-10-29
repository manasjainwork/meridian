interface Props {
  playing: boolean; speed: number; idx: number; count: number; seconds: number;
  onPlay: (p: boolean) => void; onSpeed: (s: number) => void; onSeek: (i: number) => void;
}
export default function Controls(p: Props) {
  return (
    <div className="controls">
      <button onClick={() => p.onPlay(!p.playing)}>{p.playing ? "Pause" : "Play"}</button>
      {[0.5, 1, 2, 5].map((s) => (
        <button key={s} className={p.speed === s ? "on" : ""} onClick={() => p.onSpeed(s)}>{s}x</button>
      ))}
      <input type="range" min={0} max={p.count - 1} value={p.idx} onChange={(e) => p.onSeek(Number(e.target.value))} />
      <span className="muted">{(p.seconds).toFixed(1)} s</span>
    </div>
  );
}

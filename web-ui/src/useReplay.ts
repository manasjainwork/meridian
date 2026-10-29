import { useCallback, useEffect, useRef, useState } from "react";
import type { Replay } from "./types";

// Plays a recorded feed: a clock advances by (real time * speed); the frame shown is the last one with t <= clock.
export function useReplay(replay: Replay | null) {
  const [idx, setIdx] = useState(0);
  const [playing, setPlaying] = useState(true);
  const [speed, setSpeed] = useState(1);
  const clock = useRef(0);
  const last = useRef(0);

  const find = useCallback((t: number) => {
    const f = replay!.frames;
    let lo = 0, hi = f.length - 1;
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (f[mid].t <= t) lo = mid; else hi = mid - 1; }
    return lo;
  }, [replay]);

  useEffect(() => {
    if (!replay || !playing) return;
    let raf = 0;
    last.current = performance.now();
    const loop = (now: number) => {
      clock.current += (now - last.current) * speed;
      last.current = now;
      const end = replay.frames[replay.frames.length - 1].t;
      if (clock.current > end) clock.current = 0;      // loop the recording
      setIdx(find(clock.current));
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [replay, playing, speed, find]);

  const seek = (i: number) => { if (!replay) return; clock.current = replay.frames[i].t; setIdx(i); };
  return { idx, playing, speed, setPlaying, setSpeed, seek };
}

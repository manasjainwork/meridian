#!/usr/bin/env python3
"""Turn a Kraken v2 recording (book, and optionally trade) into a replay file for the web UI.

    python3 tools/feed/to_replay.py data/kraken.jsonl web-ui/public/replay.json [--max-gap-ms 500]

frames: one per book message {t: ms since the first book message, b: [[price, qty] x15 best bids], a: same for asks}.
trades: one per live trade message {t, s: "buy"|"sell", p: price, q: qty}; the snapshot of old trades Kraken sends first is skipped.
--min-dt-ms keeps one frame per that many ms (each frame is a full book, so the file gets smaller and the picture is the same).
--max-gap-ms shortens any silence in the recording longer than that (for a demo); the replay file says so ("gaps_capped_ms").
Prices and quantities become floats here because the UI only draws them; the exact-decimal work is in check_book.py and the C++ mirror.
"""
import argparse, json, os, sys
from decimal import Decimal

sys.path.insert(0, os.path.dirname(__file__))
from check_book import cut  # noqa: E402

TOP = 15


def main(src, dst, cap, min_dt):
    depth, books, frames, trades, t0, symbol = 10, {}, [], [], None, ""
    with open(src) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                continue
            rx, _, raw = line.partition("\t")
            try:
                m = json.loads(raw, parse_float=Decimal)
            except ValueError:
                continue
            if m.get("method") == "subscribe" and m.get("success") and m.get("result", {}).get("channel") == "book":
                depth = int(m["result"].get("depth", depth)); symbol = m["result"].get("symbol", symbol)
                continue
            if m.get("channel") == "trade" and m.get("type") == "update" and t0 is not None:
                for d in m["data"]:
                    trades.append({"t": (int(rx) - t0) / 1e6, "s": d["side"], "p": float(d["price"]), "q": float(d["qty"])})
                continue
            if m.get("channel") != "book":
                continue
            for d in m["data"]:
                sym = d["symbol"]
                if m["type"] == "snapshot":
                    books[sym] = ({}, {})
                elif sym not in books:
                    continue
                bids, asks = books[sym]
                for side, lv in ((bids, d["bids"]), (asks, d["asks"])):
                    for x in lv:
                        if x["qty"] == 0: side.pop(x["price"], None)
                        else: side[x["price"]] = x["qty"]
                cut(bids, depth, True); cut(asks, depth, False)
                if not bids or not asks:
                    continue
                t0 = t0 if t0 is not None else int(rx)
                frames.append({"t": (int(rx) - t0) / 1e6,
                               "b": [[float(p), float(q)] for p, q in sorted(bids.items(), reverse=True)[:TOP]],
                               "a": [[float(p), float(q)] for p, q in sorted(asks.items())[:TOP]]})
                symbol = symbol or sym
    if cap > 0:                                        # shorten long silences, the same way for frames and trades
        gaps = [(frames[i]["t"], frames[i + 1]["t"]) for i in range(len(frames) - 1) if frames[i + 1]["t"] - frames[i]["t"] > cap]
        def squeeze(x):
            shift = 0.0
            for a, b in gaps:
                if x >= b: shift += (b - a) - cap
                elif x > a: shift += max(0.0, (x - a) - cap)
            return x - shift
        for o in frames + trades: o["t"] = squeeze(o["t"])
    for o in frames + trades: o["t"] = round(o["t"], 1)
    if min_dt > 0:                                     # keep one frame per min_dt ms; every frame is a full book, so nothing in between is needed
        kept = [frames[0]]
        for fr in frames[1:]:
            if fr["t"] - kept[-1]["t"] >= min_dt: kept.append(fr)
        if kept[-1] is not frames[-1]: kept.append(frames[-1])
        frames = kept
    out = {"symbol": symbol, "depth": depth, "source": os.path.basename(src), "frames": frames, "trades": trades}
    if cap > 0: out["gaps_capped_ms"] = cap
    if min_dt > 0: out["sampled_ms"] = min_dt
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "w") as g:
        json.dump(out, g, separators=(",", ":"))
    print(f"{len(frames)} frames, {len(trades)} trades, {frames[-1]['t'] / 1000:.1f} s, symbol {symbol}, depth {depth} -> {dst}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording"); ap.add_argument("out"); ap.add_argument("--max-gap-ms", type=float, default=0); ap.add_argument("--min-dt-ms", type=float, default=0)
    a = ap.parse_args()
    main(a.recording, a.out, a.max_gap_ms, a.min_dt_ms)

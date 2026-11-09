#!/usr/bin/env python3
"""Cut recorded Kraken files into segments (a snapshot or a '#' marker starts a new one) and write
tick tables. Nothing is interpolated or filled across segments.
Usage: python3 tools/ml/segment.py [--out data/ml] data/live/*.jsonl.gz
Writes: <out>/book_ticks.csv.gz  (one row per book message after the snapshot)
        <out>/trades.csv.gz      <out>/segments.csv"""
import argparse, csv, gzip, json, os, statistics, sys
from decimal import Decimal

def apply(side, levels, depth, reverse):
    for lv in levels:
        p, q = lv["price"], lv["qty"]
        if q == 0: side.pop(p, None)
        else: side[p] = q
    if len(side) > depth:
        for p in sorted(side, reverse=reverse)[depth:]: del side[p]

ap = argparse.ArgumentParser()
ap.add_argument("--out", default="data/ml")
ap.add_argument("--depth", type=int, default=25)
ap.add_argument("files", nargs="+")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
bt = gzip.open(f"{a.out}/book_ticks.csv.gz", "wt", newline="")
tt = gzip.open(f"{a.out}/trades.csv.gz", "wt", newline="")
bw, tw = csv.writer(bt), csv.writer(tt)
bw.writerow("seg ts_ns bid ask mid spread bid_q1 ask_q1 imb5".split())
tw.writerow("seg ts_ns side price qty".split())
segs, seg, cur = [], -1, None
def close():
    global cur
    if cur and cur["n"] > 0: segs.append(cur)
    cur = None
def new(fname, ts):
    global seg, cur
    close(); seg += 1
    cur = dict(seg=seg, file=fname, start_ns=ts, end_ns=ts, n=0, trades=0)
for path in a.files:
    fname = path.split("/")[-1]
    op = gzip.open if path.endswith(".gz") else open
    bids, asks, have = {}, {}, False
    with op(path, "rt") as f:
        for line in f:
            if line.startswith("#"):
                have = False; close(); continue
            try:
                ts, js = line.rstrip("\n").split("\t", 1); ts = int(ts)
                m = json.loads(js, parse_float=Decimal)
            except Exception:
                continue
            ch, ty = m.get("channel"), m.get("type")
            if ch == "trade" and have:
                for t in m.get("data", []):
                    tw.writerow([seg, ts, t["side"], t["price"], t["qty"]]); cur["trades"] += 1
                continue
            if ch != "book" or ty not in ("snapshot", "update"): continue
            d = m["data"][0]
            if ty == "snapshot":
                bids.clear(); asks.clear(); new(fname, ts); have = True
            elif not have: continue
            apply(bids, d.get("bids", []), a.depth, True)
            apply(asks, d.get("asks", []), a.depth, False)
            if not bids or not asks: continue
            bp, ap_ = max(bids), min(asks)
            sb = sum(bids[p] for p in sorted(bids, reverse=True)[:5])
            sa = sum(asks[p] for p in sorted(asks)[:5])
            bw.writerow([seg, ts, bp, ap_, (bp + ap_) / 2, ap_ - bp, bids[bp], asks[ap_], (sb - sa) / (sb + sa)])
            cur["end_ns"] = ts; cur["n"] += 1
close(); bt.close(); tt.close()
with open(f"{a.out}/segments.csv", "w", newline="") as f:
    w = csv.writer(f); w.writerow(["seg", "file", "start_ns", "end_ns", "dur_s", "ticks", "trades"])
    for s in segs:
        w.writerow([s["seg"], s["file"], s["start_ns"], s["end_ns"], round((s["end_ns"] - s["start_ns"]) / 1e9, 1), s["n"], s["trades"]])
d = sorted((s["end_ns"] - s["start_ns"]) / 1e9 for s in segs)
print("segments", len(segs), "total_h", round(sum(d) / 3600, 2), "median_s", round(statistics.median(d), 1),
      "max_s", round(d[-1], 1), "under_60s", sum(1 for x in d if x < 60), "ticks", sum(s["n"] for s in segs))

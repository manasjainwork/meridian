#!/usr/bin/env python3
"""Rebuild the Kraken v2 book from recorded files and compare with Kraken's CRC32.
Usage: python3 tools/ml/book_check.py [--pp 1] [--qp 8] [--depth 25] data/live/*.jsonl.gz
A snapshot starts a new segment. Nothing is interpolated."""
import argparse, gzip, json, sys, zlib
from decimal import Decimal

def fmt(d, prec):
    return f"{d:.{prec}f}".replace(".", "").lstrip("0")

def crc(bids, asks, pp, qp):
    a = sorted(asks)[:10]
    b = sorted(bids, reverse=True)[:10]
    s = "".join(fmt(p, pp) + fmt(asks[p], qp) for p in a) + "".join(fmt(p, pp) + fmt(bids[p], qp) for p in b)
    return zlib.crc32(s.encode()) & 0xFFFFFFFF

def apply(side, levels, depth, reverse):
    for lv in levels:
        p, q = lv["price"], lv["qty"]
        if q == 0:
            side.pop(p, None)
        else:
            side[p] = q
    if len(side) > depth:
        for p in sorted(side, reverse=reverse)[depth:]:
            del side[p]

def check(path, pp, qp, depth):
    op = gzip.open if path.endswith(".gz") else open
    st = dict(lines=0, bad_json=0, snaps=0, upd=0, ok=0, bad=0, trades=0, upd_before_snap=0)
    bids, asks, have, first_bad = {}, {}, False, None
    with op(path, "rt") as f:
        for line in f:
            st["lines"] += 1
            if line.startswith("#"):
                have = False
                continue
            try:
                ts, js = line.rstrip("\n").split("\t", 1)
                m = json.loads(js, parse_float=Decimal)
            except Exception:
                st["bad_json"] += 1
                continue
            ch, ty = m.get("channel"), m.get("type")
            if ch == "trade":
                st["trades"] += len(m.get("data", []))
            if ch != "book" or ty not in ("snapshot", "update"):
                continue
            d = m["data"][0]
            if ty == "snapshot":
                bids.clear(); asks.clear()
                st["snaps"] += 1
                have = True
            elif not have:
                st["upd_before_snap"] += 1
                continue
            else:
                st["upd"] += 1
            apply(bids, d.get("bids", []), depth, True)
            apply(asks, d.get("asks", []), depth, False)
            if "checksum" in d:
                if crc(bids, asks, pp, qp) == d["checksum"]:
                    st["ok"] += 1
                else:
                    st["bad"] += 1
                    if first_bad is None:
                        first_bad = ts
    return st, first_bad

ap = argparse.ArgumentParser()
ap.add_argument("--pp", type=int, default=1)
ap.add_argument("--qp", type=int, default=8)
ap.add_argument("--depth", type=int, default=25)
ap.add_argument("files", nargs="+")
a = ap.parse_args()
tot_ok = tot_bad = 0
for p in a.files:
    st, fb = check(p, a.pp, a.qp, a.depth)
    tot_ok += st["ok"]; tot_bad += st["bad"]
    print(p.split("/")[-1], st, "first_bad_ns=", fb)
print("TOTAL checksum ok", tot_ok, "bad", tot_bad)

#!/usr/bin/env python3
"""Rebuild the Kraken v2 order book from a recording and check every update against the exchange's own checksum.

    python3 tools/feed/check_book.py data/kraken-btc-30s.jsonl

Rules used (from Kraken's book-checksum-v2 guide): a snapshot replaces the book; an update sets a level, and qty 0 removes
it; after each message the book is cut to the subscribed depth; the checksum is CRC32 over the top 10 asks (low to high)
then the top 10 bids (high to low), each level written as price and qty with the decimal point and leading zeros removed.
Prices and quantities are read as exact decimals, never as floats.
If every checksum matches, the feed was understood correctly and the rebuilt book is the exchange's book.
"""
import json, sys, zlib
from decimal import Decimal


def txt(x):
    return format(x, "f") if isinstance(x, Decimal) else str(x)


def level_str(price, qty):
    return txt(price).replace(".", "").lstrip("0") + txt(qty).replace(".", "").lstrip("0")


def checksum(bids, asks):
    a = "".join(level_str(p, q) for p, q in sorted(asks.items())[:10])
    b = "".join(level_str(p, q) for p, q in sorted(bids.items(), reverse=True)[:10])
    return zlib.crc32((a + b).encode()) & 0xFFFFFFFF


def cut(side, depth, best_high):
    if len(side) > depth:
        keep = sorted(side, reverse=best_high)[:depth]
        for p in [p for p in side if p not in keep]:
            del side[p]


def main(path):
    depth = 10
    books = {}                                  # symbol -> (bids, asks)
    n_snap = n_upd = ok = bad = 0
    first_bad = []
    first_rx = last_rx = None
    markers = []
    with open(path) as f:
        for line_no, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if line.startswith("#"):
                markers.append((line_no, line.split("\t")[0]))
                continue
            rx, _, raw = line.partition("\t")
            try:
                m = json.loads(raw, parse_float=Decimal)
            except ValueError:
                continue
            if m.get("method") == "subscribe" and m.get("success") and "result" in m:
                depth = int(m["result"].get("depth", depth))
                continue
            if m.get("channel") != "book":
                continue
            for d in m["data"]:
                sym = d["symbol"]
                if m["type"] == "snapshot":
                    books[sym] = ({}, {})
                    n_snap += 1
                elif sym not in books:
                    continue                    # update before any snapshot: cannot be checked
                else:
                    n_upd += 1
                bids, asks = books[sym]
                for side, lv in ((bids, d["bids"]), (asks, d["asks"])):
                    for x in lv:
                        if x["qty"] == 0:
                            side.pop(x["price"], None)
                        else:
                            side[x["price"]] = x["qty"]
                cut(bids, depth, True)
                cut(asks, depth, False)
                want = int(d["checksum"])
                got = checksum(bids, asks)
                if m["type"] == "update":
                    if got == want:
                        ok += 1
                    else:
                        bad += 1
                        if len(first_bad) < 3:
                            first_bad.append((line_no, want, got, sorted(bids.items(), reverse=True)[:3], sorted(asks.items())[:3]))
                rxn = int(rx)
                first_rx = first_rx or rxn
                last_rx = rxn
    secs = (last_rx - first_rx) / 1e9 if first_rx else 0
    print(f"depth {depth}; snapshots {n_snap}; updates {n_upd} ({n_upd / secs:.1f}/s over {secs:.1f} s)" if secs else "no book messages")
    print(f"checksum match {ok}, mismatch {bad}")
    for ln, want, got, b, a in first_bad:
        print(f"  line {ln}: exchange {want}, mine {got}; top bids {[(txt(p), txt(q)) for p, q in b]} top asks {[(txt(p), txt(q)) for p, q in a]}")
    print("markers:", ", ".join(f"{t}@{n}" for n, t in markers) or "none")
    sys.exit(0 if bad == 0 and ok > 0 else 1)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check_book.py RECORDING.jsonl")
    main(sys.argv[1])

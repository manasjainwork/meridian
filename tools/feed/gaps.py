#!/usr/bin/env python3
"""Report time gaps in recorded Kraken files. Usage: gaps.py [--min SECONDS] FILE.jsonl[.gz] ...
Each data line is '<unix ns><TAB><json>'; lines starting with '#' are markers and are counted, not timed."""
import gzip, sys, datetime

def opener(p):
    return gzip.open(p, "rt", errors="replace") if p.endswith(".gz") else open(p, errors="replace")

def utc(ns):
    return datetime.datetime.fromtimestamp(ns / 1e9, datetime.timezone.utc).strftime("%m-%d %H:%M:%S")

def main(argv):
    thr = 5.0
    if argv and argv[0] == "--min":
        thr = float(argv[1]); argv = argv[2:]
    tot_gap = 0.0
    print(f"{'file':44} {'lines':>8} {'marks':>5} {'span_s':>8} {'max_gap_s':>9} {'max_gap_at':>12} {'gaps>='+str(thr):>8} {'gap_total_s':>11}")
    for p in argv:
        n = marks = 0; first = last = prev = None; mx = 0.0; mx_at = 0; ng = 0; gsum = 0.0
        with opener(p) as f:
            for line in f:
                if line.startswith("#"):
                    marks += 1; continue
                tab = line.find("\t")
                if tab <= 0: continue
                try: t = int(line[:tab])
                except ValueError: continue
                n += 1
                if first is None: first = t
                if prev is not None:
                    g = (t - prev) / 1e9
                    if g > mx: mx, mx_at = g, prev
                    if g >= thr: ng += 1; gsum += g
                prev = t; last = t
        span = (last - first) / 1e9 if first else 0.0
        tot_gap += gsum
        print(f"{p.split('/')[-1]:44} {n:8d} {marks:5d} {span:8.0f} {mx:9.1f} {utc(mx_at) if mx_at else '-':>12} {ng:8d} {gsum:11.0f}")
    print(f"total gap seconds (gaps >= {thr}s): {tot_gap:.0f}")

if __name__ == "__main__":
    main(sys.argv[1:])

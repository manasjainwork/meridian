#!/usr/bin/env python3
"""Descriptive stats of the recorded segments (calibration targets for the simulator).
Only within segments; seconds with no data are skipped, never filled. Describes these recordings only."""
import csv, gzip, math, statistics, sys
from collections import Counter, defaultdict
MIN_SEG_S = float(sys.argv[1]) if len(sys.argv) > 1 else 300
dur = {}
with open("data/ml/segments.csv") as f:
    for r in csv.DictReader(f): dur[int(r["seg"])] = float(r["dur_s"])
keep = {s for s, d in dur.items() if d >= MIN_SEG_S}
print("min_seg_s", MIN_SEG_S, "segments_kept", len(keep), "hours_kept", round(sum(dur[s] for s in keep) / 3600, 2))
spread, last, nticks = Counter(), {}, defaultdict(int)
with gzip.open("data/ml/book_ticks.csv.gz", "rt") as f:
    rd = csv.reader(f); next(rd)
    for r in rd:
        s = int(r[0])
        if s not in keep: continue
        sec = int(r[1]) // 10**9
        spread[round(float(r[5]) / 0.1)] += 1
        last[(s, sec)] = float(r[4]); nticks[s] += 1
tot = sum(spread.values()); acc = 0; q = {}
for k in sorted(spread):
    acc += spread[k]
    for p in (0.5, 0.9, 0.99):
        if p not in q and acc / tot >= p: q[p] = k
print("spread_in_0.1_units p50/p90/p99", q[0.5], q[0.9], q[0.99])
print("book_msgs_per_s", round(sum(nticks.values()) / sum(dur[s] for s in keep), 1))
rets, zero = [], 0
for (s, sec), m in last.items():
    p = last.get((s, sec - 1))
    if p:
        rets.append(math.log(m / p)); zero += (m == p)
n = len(rets); sd = statistics.pstdev(rets)
print("1s_returns", n, "std_bp", round(sd * 1e4, 3), "frac_zero", round(zero / n, 3))
print("1s_return_excess_kurtosis", round(sum(((x - 0) / sd) ** 4 for x in rets) / n - 3, 1))
sizes, signs = [], defaultdict(list)
with gzip.open("data/ml/trades.csv.gz", "rt") as f:
    rd = csv.reader(f); next(rd)
    for r in rd:
        s = int(r[0])
        if s not in keep: continue
        sizes.append(float(r[4])); signs[s].append(1 if r[2] == "buy" else -1)
sizes.sort()
print("trades", len(sizes), "per_s", round(len(sizes) / sum(dur[s] for s in keep), 3), "size_btc p50/p90/p99",
      sizes[len(sizes) // 2], sizes[int(len(sizes) * .9)], sizes[int(len(sizes) * .99)])
same = pairs = 0
for v in signs.values():
    for a, b in zip(v, v[1:]): pairs += 1; same += (a == b)
print("trade_sign_same_as_prev", round(same / pairs, 3), "(0.5 = no memory)")

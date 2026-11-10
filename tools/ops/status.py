#!/usr/bin/env python3
"""Write web-ui/public/status.json (aggregate numbers only) for the Research tab.
Inputs: data/ml/segments.csv, data/ml/stats.txt, data/book_check_report.txt, data/live/runner.log.
Missing inputs become null; nothing is invented."""
import csv, datetime as dt, glob, json, re, statistics

MILESTONES = [
    ("engine", "M1 Sequencer core", "partial", "Core and tests pass on the author's machine; mutation tests and a ThreadSanitizer run pending"),
    ("engine", "M2 Event log v2 and replay", "todo", ""),
    ("engine", "M3 Binary TCP gateway v2", "todo", ""),
    ("engine", "M4 Timing (T1 to T4, histograms)", "todo", ""),
    ("engine", "M5 Virtual-clock mode", "todo", ""),
    ("engine", "M6 Downstream publisher", "todo", ""),
    ("ml", "Kraken recorder with auto-start and gap report", "done", "Records only while the laptop is on"),
    ("ml", "Book rebuild checked against Kraken's CRC32", "done", ""),
    ("ml", "Segmenter and descriptive stats", "done", "Describes these recordings only"),
    ("ml", "Several days of recording including a weekend", "partial", "In progress"),
    ("ml", "Simulator calibrated to the recordings", "todo", ""),
    ("ml", "Baseline agents", "todo", ""),
    ("ml", "First RL agent", "todo", ""),
    ("ml", "Latency experiments (H1 to H4)", "todo", ""),
]
DOUBTS = [
    "Why the Kraken connection resets so often (median segment 20 s)",
    "Why 2 to 4 percent of book messages show a spread above 1 USD",
    "Whether the 85 percent same-side trade rate is one order printed as several trades",
    "SPSC throughput is bimodal on WSL2 with no known cause",
    "Millisecond stalls on WSL2",
    "Every latency number needs bare-metal Linux before it is a claim",
]

def read(p):
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return None

out = {"generated_at": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
       "milestones": [dict(group=g, name=n, state=s, note=t) for g, n, s, t in MILESTONES], "doubts": DOUBTS}
try:
    with open("data/ml/segments.csv") as f:
        rows = list(csv.DictReader(f))
except OSError:
    rows = []
if rows:
    d = sorted(float(r["dur_s"]) for r in rows)
    edges = [("under 10 s", 0, 10), ("10 to 60 s", 10, 60), ("1 to 5 min", 60, 300), ("5 to 30 min", 300, 1800), ("over 30 min", 1800, 1e12)]
    ts = lambda ns: dt.datetime.fromtimestamp(ns / 1e9, dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    out["segments"] = {"count": len(d), "hours": round(sum(d) / 3600, 2), "median_s": round(statistics.median(d), 1), "max_s": round(d[-1], 1),
                       "ticks": sum(int(r["ticks"]) for r in rows), "trades": sum(int(r["trades"]) for r in rows),
                       "from": ts(min(int(r["start_ns"]) for r in rows)), "to": ts(max(int(r["end_ns"]) for r in rows)),
                       "buckets": [{"label": l, "count": sum(1 for x in d if lo <= x < hi)} for l, lo, hi in edges]}
rep = read("data/book_check_report.txt")
if rep:
    m = re.search(r"TOTAL checksum ok (\d+) bad (\d+)", rep)
    files = sum(1 for ln in rep.splitlines() if "'ok':" in ln and int(re.search(r"'ok': (\d+)", ln).group(1)) > 0)
    if m:
        out["checksums"] = {"ok": int(m.group(1)), "bad": int(m.group(2)), "files": files}
st = read("data/ml/stats.txt")
if st:
    out["stats"] = [{"k": ln.split(" ", 1)[0], "v": ln.split(" ", 1)[1] if " " in ln else ""} for ln in st.splitlines() if ln.strip()]
log = read("data/live/runner.log")
out["recorder"] = {"files_gz": len(glob.glob("data/live/*.jsonl.gz")),
                   "last_log_line": log.strip().splitlines()[-1] if log and log.strip() else None}
with open("web-ui/public/status.json", "w") as f:
    json.dump(out, f, indent=1)
print("wrote web-ui/public/status.json:", ", ".join(k for k in out if out[k] is not None))

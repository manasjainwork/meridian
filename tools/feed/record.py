#!/usr/bin/env python3
"""Record a public market-data WebSocket feed to a file, untouched.

    python3 tools/feed/record.py --seconds 30 --products BTC-USD --out data/btc-30s.jsonl

Each output line is:  <receive time in ns since epoch> TAB <the raw message exactly as received>
Lines starting with '#' are markers written by this tool (start, reconnect, end).
Nothing is parsed or changed: later tools (normalizer, replayer, backtester) read this file, so the same recording can be
replayed any number of times. At the end a summary shows how many messages of each type arrived.
"""
import argparse, asyncio, collections, json, sys, time

import websockets

DEFAULT_URL = "wss://ws-feed.exchange.coinbase.com"


async def record(args):
    counts = collections.Counter()
    first = {}
    total = 0
    deadline = time.monotonic() + args.seconds
    attempt = 0
    with open(args.out, "w", buffering=1) as f:
        f.write(f"#start\t{time.time_ns()}\turl={args.url}\tproducts={','.join(args.products)}\tchannels={','.join(args.channels)}\n")
        while time.monotonic() < deadline and attempt <= args.retries:
            try:
                async with websockets.connect(args.url, max_size=None, ping_interval=20) as ws:
                    # The server disconnects a client that does not subscribe within 5 seconds, so subscribe at once.
                    if args.subscribe != "none":
                        msg = args.subscribe or json.dumps({"type": "subscribe", "product_ids": args.products, "channels": args.channels})
                        await ws.send(msg)
                    attempt = 0
                    while True:
                        left = deadline - time.monotonic()
                        if left <= 0:
                            break
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=min(left, 10))
                        except asyncio.TimeoutError:
                            if time.monotonic() >= deadline:
                                break
                            continue                       # quiet feed: keep waiting until the deadline
                        rx = time.time_ns()
                        if isinstance(raw, bytes):
                            raw = raw.decode("utf-8", "replace")
                        raw = raw.replace("\n", " ").replace("\r", " ")
                        f.write(f"{rx}\t{raw}\n")
                        total += 1
                        try:
                            j = json.loads(raw)
                            if "method" in j:
                                t = "ack:" + str(j["method"])
                            elif "channel" in j:
                                t = str(j["channel"]) + ":" + str(j.get("type", ""))
                            else:
                                t = j.get("type", "?")
                        except Exception:
                            t = "not-json"
                        counts[t] += 1
                        first.setdefault(t, raw)
            except (OSError, websockets.exceptions.WebSocketException) as e:
                attempt += 1
                f.write(f"#error\t{time.time_ns()}\t{type(e).__name__}: {e}\n")
                print(f"connection problem ({type(e).__name__}: {e}); retry {attempt}/{args.retries}", file=sys.stderr)
                await asyncio.sleep(min(2 ** attempt, 10))
                f.write(f"#reconnect\t{time.time_ns()}\n")
        f.write(f"#end\t{time.time_ns()}\tmessages={total}\n")
    return total, counts, first


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default=DEFAULT_URL)
    p.add_argument("--products", nargs="+", default=["BTC-USD"])
    p.add_argument("--channels", nargs="+", default=["level2", "ticker", "heartbeat"])
    p.add_argument("--subscribe", default=None, help="raw JSON subscribe message to send instead of the Coinbase one; 'none' sends nothing")
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--out", default="data/feed.jsonl")
    p.add_argument("--retries", type=int, default=3)
    args = p.parse_args()
    total, counts, first = asyncio.run(record(args))
    print(f"\nrecorded {total} messages to {args.out}")
    for t, n in counts.most_common():
        print(f"  {t:<14} {n:>8}")
    print("\nfirst message of each type (cut at 400 characters):")
    for t, raw in first.items():
        print(f"  [{t}] {raw[:400]}")
    if total == 0:
        print("\nNo messages arrived: check the URL, your network or region, and the channel names.", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

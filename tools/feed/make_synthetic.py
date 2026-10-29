#!/usr/bin/env python3
"""Make a synthetic recording in the Kraken v2 book format, for testing the mirror without a network.

    python3 tools/feed/make_synthetic.py data/synthetic.jsonl [seed]

The exchange side is a small model written independently of the C++ engine: it keeps the book as exact decimals, cuts it to depth
25, and computes Kraken's checksum with zlib.crc32 (tools/feed/check_book.py, which matched the real exchange on 272 of 272
updates). The recording includes the cases a mirror can get wrong: a level larger than 2^32 lots, many levels added and
removed at once, a message in which a bid is added at a price where an ask is removed (the book would cross if the additions
were applied first), levels that fall out of the depth without a zero-quantity message, and a second snapshot (reconnect).
"""
import json, os, random, sys
from decimal import Decimal

sys.path.insert(0, os.path.dirname(__file__))
from check_book import checksum, cut, txt  # noqa: E402

DEPTH = 25
TICK = Decimal("0.1")
LOT = Decimal("0.00000001")


def num(x):
    return txt(x)


def side_json(levels):
    return "[" + ",".join('{"price":%s,"qty":%s}' % (num(p), num(q)) for p, q in levels) + "]"


def rq(rnd, hi):
    return (Decimal(rnd.randint(1, hi)) * LOT).quantize(LOT)


def main(path, seed):
    rnd = random.Random(seed)
    bids, asks = {}, {}
    base = Decimal("79820.0")
    for i in range(DEPTH + 10):
        bids[base - TICK * i] = rq(rnd, 500000)
        asks[base + TICK * (i + 1)] = rq(rnd, 500000)
    cut(bids, DEPTH, True); cut(asks, DEPTH, False)
    bids[base] = Decimal("50.12345678")                  # larger than 2^32 lots (42.9 BTC): needs two engine orders
    t = 1_000_000_000
    out = open(path, "w")
    out.write("#start\t1\n")

    def emit(obj_text):
        nonlocal t
        t += rnd.randint(1_000_000, 30_000_000)
        out.write(f"{t}\t{obj_text}\n")

    def snapshot():
        emit('{"method":"subscribe","result":{"channel":"book","depth":25,"snapshot":true,"symbol":"BTC/USD"},"success":true}')
        emit('{"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD","bids":%s,"asks":%s,"checksum":%d}]}' % (
            side_json(sorted(bids.items(), reverse=True)), side_json(sorted(asks.items())), checksum(bids, asks)))

    def update(bch, ach):
        for p, q in bch: (bids.pop(p, None) if q == 0 else bids.__setitem__(p, q))
        for p, q in ach: (asks.pop(p, None) if q == 0 else asks.__setitem__(p, q))
        cut(bids, DEPTH, True); cut(asks, DEPTH, False)
        emit('{"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":%s,"asks":%s,"checksum":%d,"timestamp":"2026-10-29T04:26:34.620757Z"}]}' % (
            side_json(bch), side_json(ach), checksum(bids, asks)))

    snapshot()
    for step in range(3000):
        if len(bids) < 12:                                # an exchange book refills: keep both sides populated
            low = min(bids); update([(low - TICK * (i + 1), rq(rnd, 900000)) for i in range(8)], [])
        if len(asks) < 12:
            top = max(asks); update([], [(top + TICK * (i + 1), rq(rnd, 900000)) for i in range(8)])
        r = rnd.random()
        bb, ba = max(bids), min(asks)
        if step == 1000:                                  # crossing trap: remove two asks, add a bid at a removed ask's price
            a1, a2 = sorted(asks)[:2]
            update([(a1, Decimal("1.50000000"))], [(a1, Decimal(0)), (a2, Decimal(0))])
        elif step == 2000:
            snapshot()                                    # reconnect: a fresh snapshot of the current book
        elif r < 0.35:                                    # change a level inside the book
            side = rnd.choice([bids, asks]); p = rnd.choice(sorted(side)[:12])
            ch = [(p, (Decimal(rnd.randint(0, 800000)) * LOT).quantize(LOT))]
            update(ch if side is bids else [], ch if side is asks else [])
        elif r < 0.60:                                    # add a new level just outside the best
            if rnd.random() < 0.5:
                p = bb - TICK * rnd.randint(1, 30); update([(p, rq(rnd, 900000))], [])
            else:
                p = ba + TICK * rnd.randint(1, 30); update([], [(p, rq(rnd, 900000))])
        elif r < 0.75:                                    # remove the best level: a trade ate it
            if rnd.random() < 0.5: update([(bb, Decimal(0))], [])
            else: update([], [(ba, Decimal(0))])
        elif r < 0.85:                                    # the market moves: remove several top levels, add several new ones
            k = rnd.randint(2, 6)
            if rnd.random() < 0.5:
                gone = sorted(asks)[:k]; top = max(asks)
                update([], [(p, Decimal(0)) for p in gone] + [(top + TICK * (i + 1), rq(rnd, 500000)) for i in range(k)])
            else:
                gone = sorted(bids, reverse=True)[:k]; low = min(bids)
                update([(p, Decimal(0)) for p in gone] + [(low - TICK * (i + 1), rq(rnd, 500000)) for i in range(k)], [])
        else:                                             # small size changes at the best
            p = rnd.choice([bb, ba]); ch = [(p, rq(rnd, 100000))]
            update(ch if p == bb else [], ch if p == ba else [])
        if min(asks) <= max(bids):                        # keep the synthetic exchange uncrossed
            sys.exit(f"generator bug: crossed at step {step}")
    out.write("#end\t9\n")
    out.close()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "data/synthetic.jsonl", int(sys.argv[2]) if len(sys.argv) > 2 else 1)

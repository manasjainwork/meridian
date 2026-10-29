#!/usr/bin/env python3
"""Turn a Kraken v2 book recording into plain integer events for the C++ mirror (build/mirror_check).

    python3 tools/feed/normalize.py data/kraken-btc-30s.jsonl --price-decimals 1 --qty-decimals 8 > data/kraken-btc-30s.ev

Output lines (all numbers are integers; prices in ticks of 10^-price_decimals, quantities in lots of 10^-qty_decimals):
    D <depth>                      subscribed depth
    R                              a snapshot starts: forget the book
    L <b|a> <ticks> <lots>         set one level (lots 0 removes it)
    E <s|u> <checksum> <rx_ns>     the message is complete: apply it and check
The scaling is verified: a price or quantity with more decimals than declared stops the run with an error.
"""
import argparse, json, sys
from decimal import Decimal

short_format = {"price": 0, "qty": 0}


def scaled(x, decimals, what, line_no):
    d = Decimal(str(x)) if not isinstance(x, Decimal) else x
    # The checksum is computed from the text the exchange sent. If a value arrives with FEWER decimals than declared
    # (79824 instead of 79824.0), the engine's integer would produce a different checksum string. Count those.
    if d != 0 and d.as_tuple().exponent != -decimals:
        short_format[what] += 1
    v = d.scaleb(decimals)
    if v != v.to_integral_value():
        sys.exit(f"line {line_no}: {what} {format(d, 'f')} has more than {decimals} decimals")
    return int(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording")
    ap.add_argument("--price-decimals", type=int, required=True)
    ap.add_argument("--qty-decimals", type=int, required=True)
    a = ap.parse_args()
    out = sys.stdout
    with open(a.recording) as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if line.startswith("#"):
                continue
            rx, _, raw = line.partition("\t")
            try:
                m = json.loads(raw, parse_float=Decimal)
            except ValueError:
                continue
            if m.get("method") == "subscribe" and m.get("success") and m.get("result", {}).get("channel") == "book":
                out.write(f"D {int(m['result'].get('depth', 10))}\n")
                continue
            if m.get("channel") != "book":
                continue
            for d in m["data"]:
                if m["type"] == "snapshot":
                    out.write("R\n")
                for side, key in (("b", "bids"), ("a", "asks")):
                    for x in d[key]:
                        out.write(f"L {side} {scaled(x['price'], a.price_decimals, 'price', n)} {scaled(x['qty'], a.qty_decimals, 'qty', n)}\n")
                out.write(f"E {'s' if m['type'] == 'snapshot' else 'u'} {int(d['checksum'])} {rx}\n")


if __name__ == "__main__":
    main()
    if short_format["price"] or short_format["qty"]:
        print(f"WARNING: values not written with the declared number of decimals: prices {short_format['price']}, quantities {short_format['qty']}. "
              "The exchange checksum may then differ from the engine's.", file=sys.stderr)

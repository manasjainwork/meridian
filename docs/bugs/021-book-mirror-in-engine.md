# 021: mirroring a real exchange book inside the engine

## What was built
- `tools/feed/normalize.py`: Kraken v2 recording to integer events (price in ticks of 0.1, quantity in lots of 1e-8). It stops if a value has more decimals than declared and warns if a value was written with fewer.
- `src/mirror_check.cpp`: keeps a plain `std::map` copy of the exchange book, cuts it to the subscribed depth (25), and brings the engine's `OrderBook` to the same state with cancel and submit calls. One exchange price level is one resting order (more than one if it holds more than 2^32-1 lots). Removals are applied before additions, so the book never crosses. After each message the top 10 levels are read back out of the engine, Kraken's checksum string is built from them, and its CRC32 is compared with the checksum the exchange sent.
- `tools/feed/make_synthetic.py` and `scripts/mirror_test.sh`: synthetic recordings with the awkward cases (a level above 2^32 lots, a bid added at a price where an ask is removed in the same message, a mid-recording snapshot), 8 seeds.

## Results (from my machine, WSL2)
- Synthetic, 8 seeds: Python checker and engine both match every checksum. Applying additions before removals (`--naive`) is caught in 8 of 8 seeds, so the order matters and the test would notice.
- Real recording `data/kraken-btc-120s.jsonl` (BTC/USD, depth 25, 118.8 s, 1 snapshot, 12397 updates, no reconnect): `check_book.py` 12397 match / 0 mismatch. Engine mirror: snapshot 1 of 1 and updates 12397 of 12397 match, 0 level quantity differences, 0 best bid/ask differences, 0 unexpected fills, 0 rejected calls, 0 invariant failures.

## Limits (not established)
- The checksum covers the top 10 levels only, the book is 25 deep (doubts 13).
- One recording of about two minutes, one run. Engine call timing (24842 calls, p50 20 ns, p99 598 ns, max 27537 ns) is one run on WSL2 and is not a benchmark.
- Prices up to 2,000,000 ticks (200,000.0) only; a higher price is counted as out of range.
- This mirrors the book; it does not match against it. The engine never sees the exchange's own trades.

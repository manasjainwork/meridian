# 006: Order book baseline

## Design
Price-time priority limit book. Integer price ticks, fixed order pool (no allocation on add or
cancel), FIFO list per price level, open-addressing id map with backward-shift deletion.

## Correctness
- Directed tests: priority, maker-price fills, multi-level sweep, partial fills, cancel,
  duplicate id, bad parameters, pool exhaustion and slot reuse.
- Differential fuzz: 20 rounds x 20,000 random operations against a std::map reference book.
  Fills, best bid/ask, order count and per-level quantity compared after every operation.
- Same tests clean under AddressSanitizer and UBSan.

## Setup
400,000 operations (30% cancels, limits within +-20 ticks of mid), fixed seed.
WSL2, g++ 15.2, Release, TSC 2.688 GHz, 32-cycle timer overhead subtracted.

## Result (ns)
| op     | n      | min | p50  | p99   | p99.9 | max     |
|--------|--------|-----|------|-------|-------|---------|
| rest   | 170692 | 8.9 | 23.1 | 129.5 | 448.7 | 55116.2 |
| match  | 109310 | 9.7 | 38.7 | 147.3 | 447.2 | 13557.3 |
| cancel | 119998 | 5.2 | 23.1 | 162.2 | 468.8 | 19441.3 |

fills=198016, resting=45590, best bid/ask 1007/1008, checksum 34382 (identical on two machines).

## Observations
- Median cost is 23-39 ns per operation.
- p99 is 5-7x the median and p99.9 is ~20x. Hypothesis: cache misses on the order pool
  and a linear scan in level_emptied. Not yet measured.
- max is dominated by WSL2 preemption (see 001), not the book.

## Next
- Profile with perf to find where p99 comes from.
- Replace the linear best-price scan with a bitmap, then compare before/after with the same seed.

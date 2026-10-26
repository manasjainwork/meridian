# 007: Linear best-price scan replaced by hierarchical bitmap

## Problem
When the best level empties, the book scanned price by price to find the next one.
Cost grows with the gap between levels, so a sparse book is the worst case.

## Change
`PriceBitmap`: 3-level bitset, one word probe per level (`countr_zero` / `countl_zero`).
Old scan kept behind `MERIDIAN_BOOK_LINEAR_SCAN` for before/after runs.

## Correctness
- Bitmap property test vs `std::set` (9 sizes, 40k ops each): pass
- Differential fuzz vs `std::map` reference, now including sparse prices 1..2000: pass
- ASan/UBSan: clean
- Same fills (198016) and checksum (34382) in both modes

## Results (WSL2, TSC 2.688 GHz, ns)
Sparse sweep (gap ~59000 ticks), p50 / p99:
- sweep ask: 26478 / 57844  ->  20.1 / 21.6
- sweep bid: 27899 / 64813  ->  20.8 / 23.1

Mixed workload (4 runs each): p50 and p99 are the same within noise.
- p99 ranged ~100-130 ns for both versions; one bitmap run hit 132/169/194 ns.
- The first run (linear 211-272 ns p99) was an outlier. A single run is not evidence.

## Not explained yet
The scan was never the cause of the ~100-130 ns p99 or the ~450 ns p99.9 in the mixed workload.
The bitmap only fixes the sparse worst case. Next: perf to find the real tail source.

# 009: SPSC ring buffer vs mutex queue

## Implementation
`SpscQueue<T,N>`: power-of-two ring, acquire/release atomics, head/tail on separate cache lines,
cached copy of the other side's index. Tests: FIFO/full/empty/wraparound, 5M items across two
threads (order + sum), ThreadSanitizer clean.

## Results (WSL2, 12 hw threads, producer cpu 0 / consumer cpu 1, 3 runs, ns)
Ping-pong round trip, p50 / p99:
- spsc : 179-194 / 251-333
- mutex: 757-906 / 1734-5259
Ratio mutex/spsc: ~4-5x at p50, ~7-16x at p99.

Throughput: mutex 4.1-5.5 M items/s (stable). SPSC 20.3, 36.8, 299.1 M items/s.

## Not established
- SPSC throughput varies 15x between runs. No single number is claimed.
  Hypotheses (untested): cpu 0/1 may be hyperthread siblings; cache-line ping-pong when the ring
  stays near empty vs batching when it fills.
- SPSC p99.9 is ~1.6-1.7 us in all runs; cause unknown.

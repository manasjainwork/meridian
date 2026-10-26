# 004: TCP loopback round trip and the cost of the kernel

## Setup
exchange_sim and order_client on 127.0.0.1, TCP_NODELAY on both sides, one order in flight,
50,000 NewOrder/OrderAck round trips. WSL2, g++ 15.2, Release, TSC 2.688 GHz.

## Result
| pct   | us     |
|-------|--------|
| min   | 26.8   |
| p50   | 47.1   |
| p99   | 198.4  |
| p99.9 | 321.1  |
| max   | 1166.0 |

## Correctness
- 51,000 orders processed, seq_gaps=0.
- 1,000 orders sent as one byte stream in random 1..40 byte chunks: all acks returned in order.
- Exchange order ids stay consecutive (1300000000000000 + n), stored as uint64.

## Takeaways
- Net parse cost is ~6 ns (see 002). The round trip is ~47 us, about 8000x more.
  The kernel network path, not our code, dominates latency.
- Each side blocks in recv(), so every round trip includes two thread wakeups by the scheduler.
- Tail is heavy (p99 4x p50, max 25x p50), consistent with the WSL2 jitter seen in 001.
- Next: measure busy-polling (non-blocking recv in a spin loop, pinned core) against this baseline.

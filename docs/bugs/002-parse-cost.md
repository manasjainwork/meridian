# 002: Zero-copy parse cost

## Setup
Packed big-endian structs (#pragma pack(1)), parse_one() over a 100,000 message stream,
second pass recorded. WSL2, g++ 15.2, Release, TSC 2.688 GHz.

## Result (timer overhead of ~32 cycles included)
| pct   | cycles | ns      |
|-------|--------|---------|
| min   | 44     | 16.4    |
| p50   | 48     | 17.9    |
| p99   | 54     | 20.1    |
| p99.9 | 66     | 24.6    |
| max   | 34810  | 12950.2 |

## Takeaways
- Net parse cost is about 16 cycles (~6 ns) after subtracting timer overhead.
- Test asserts the returned header pointer equals the input buffer address: nothing is copied.
- Max is again ~700x the median: same WSL2 preemption as in 001, not a parser cost.
- TCP is a byte stream, so parse_one returns NeedMore for partial messages instead of failing.

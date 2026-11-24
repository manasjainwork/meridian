# 038: stalls of 100 ms and more through the TCP server (hypothesis supported, one controlled test)

## What was seen (author's machine, WSL2)
`tcp_latency` (open loop, 2 connections, 100000 requests per second each) against `seq_gateway` gave p50 of 24 to 36 microseconds in every run, but p99 values from 0.2 ms to 1.1 s that changed from run to run. Runs on the same server process that came later had the worst tails. An idle probe (`hiccup`, spinning threads and no engine code) saw gaps up to 12 ms on every core, with and without load, and never a gap over 100 ms. So the machine explains stalls of a few ms but not of 100 ms or more. Pinning the gateway threads to their own cores and the clients to separate physical cores (layout B) did not remove them.

## Hypothesis
The sequencer keeps every applied request in a `std::vector<Applied>` (120 bytes each). The server reserved 1,048,576 entries. When the count crosses 1,048,576, 2,097,152, 4,194,304 or 8,388,608 the vector reallocates and copies everything (about 126, 252, 504 and 1008 MB), and the sequencer thread stops for that time.

## Evidence
- Counting requests applied so far, the runs with a stall of 100 ms or more were the runs that crossed one of those counts (layout A sweep, layout B sweep, and the run with the probe); runs that did not cross had p99 of 12 ms or less. Larger counts gave longer stalls (108 ms at 1M, 240 to 342 ms at 2M, 421 to 591 ms at 4M, 952 ms to 1.1 s at 8M).
- One test of the hypothesis: a fresh server with the reserve set to 4,000,000 entries (no crossing in 3,000,000 requests), same load. p99 was 10.9, 0.66 and 0.93 ms, max 26, 6.8 and 21.7 ms; earlier runs on a fresh server with the default reserve had p99 of 108 and 342 ms in runs 2 and 3.

## Not shown yet
- The test was done once on a fresh server (3 runs on it). It was not repeated, and the reallocation time was not measured directly, for example with a timestamp around `push_back`.
- p99 of 10.9 ms in the first run after a start was also seen without a crossing (12.8 ms); first touch of the reserved memory is a guess, untested.
- Numbers are from WSL2, 2 connections, and the workload sends random orders from one account per connection, about 40 to 70 percent of them rejected as self-trade (same workload as `seq_latency`). They are observations, not benchmarks.
- p50 of 24 to 28 microseconds at 200000 requests per second in total (3 runs) is the only figure stable enough to quote; p99 and above are dominated by the machine's own hiccups.

## Fix (not done)
Store the applied log in fixed-size chunks (no copy when it grows), or write it from a separate writer thread and keep only a bounded ring in the sequencer.

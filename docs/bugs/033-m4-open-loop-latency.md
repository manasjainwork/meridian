# 033: Open-loop latency of the sequencer (M4, first measurement)

## What was built
- Sequencer::enable_timing: T2 (request popped) and T3 (apply finished) as TSC ticks on the sequencer core, one Stamp per applied request; off by default. tests/test_seq_timing.cpp (3 runs ok): one stamp per request, T2 <= T3, T3 of one request <= T2 of the next, timing does not change the checksums.
- bench/seq_latency.cpp: 2 producers on cores 1 and 2, sequencer on core 0, fixed schedule, latency measured from the time a request was due. Intervals: queue (T2 - due), engine (T3 - T2), ack (T4 - T3), total (T4 - due). Replay of the log must give the same OMS and fills checksums.

## Results (author machine, WSL2, 600,000 requests per run, 3 runs per rate, after the fix in bug 032)
Replay checksum equal in all 9 runs. No ring-full retries in any run, no reply stalls. Offered rate was completed in every run (200,000, 600,000 and 1,200,000 requests per second in total). Ranges over the 3 runs:

| offered total | engine p50 | engine p99 | engine p99.9 | total p50 | total p99 | total p99.9 | total max |
|---|---|---|---|---|---|---|---|
| 200,000/s | 409 to 495 ns | 3.8 to 4.7 us | 17.3 to 18.7 us | 1.05 to 1.22 us | 48 to 163 us | 0.96 to 4.8 ms | 3.4 to 7.3 ms |
| 600,000/s | 285 to 349 ns | 1.3 to 2.2 us | 12.7 to 17.3 us | 0.84 to 0.99 us | 58 to 643 us | 0.19 to 3.0 ms | 0.52 to 4.4 ms |
| 1,200,000/s | 255 to 283 ns | 1.1 to 1.6 us | 12.5 to 16.3 us | 0.80 to 0.99 us | 132 to 256 us | 0.29 to 0.63 ms | 0.61 to 0.77 ms |

At 600,000/s the producers waited for the contract limit (fewer than 1,024 requests without a reply) in 2 of 3 runs (207,421 and 183,871 waits) and in none in the third.
One stamp pair costs 10 to 12 ns (median 12), included in the engine interval. Queue and ack cross two cores, so they carry the TSC uncertainty of about 70 ns (bug 031); no negative values occurred.

## What this says, and what not
- The engine interval (one core, exact) is a few hundred nanoseconds at the median and about 1 to 5 microseconds at p99. The end-to-end median is about 1 microsecond.
- The tail (p99 and above of queue and total) is tens to thousands of microseconds, and at 200,000/s with a 3 second run it reaches milliseconds. The cause is not known. A guess, not tested: scheduler or hypervisor interruptions of WSL2, which would grow with run length and would not appear on bare metal with isolated cores.
- Engine p50 is lower at higher rates (about 270 ns at 1.2 million/s against about 450 ns at 200,000/s). The cause is not known (caches, frequency, idle states are guesses).
- No saturation point was reached: the sequencer kept up at 1.2 million requests per second in total. Its capacity is higher than that and is not measured.
- The order stream has no trades across accounts of interest and no stops; it is not a market model.
- WSL2: these are observations on this machine, not benchmarks.

## Not done yet
- A rate sweep to the saturation point, longer runs, T1 and T4 at the TCP gateway, and bare-metal Linux with pinned and isolated cores.

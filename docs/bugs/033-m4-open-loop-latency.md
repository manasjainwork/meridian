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
- The order stream is random limit orders and cancels from two accounts (trades between them, self-trade rejects inside one), with no stops and no market orders; it is not a market model.
- WSL2: these are observations on this machine, not benchmarks.

## Not done yet
- A rate sweep to the saturation point, longer runs, T1 and T4 at the TCP gateway, and bare-metal Linux with pinned and isolated cores.

## Sweep to saturation (author machine, WSL2, 600,000 requests per run, 3 runs per rate, replay checksum equal in 15 of 15)
Offered rates per producer 800,000 to 3,000,000 per second (1.6 to 6 million in total):

| offered total | completed per run | total latency p50 per run |
|---|---|---|
| 1.6 million/s | 1.598, 1.599, 1.600 million/s | 1.0 us, 3.9 us, 5.6 ms |
| 2.0 million/s | 1.91, 1.86, 1.79 million/s | 13.7, 23.7, 18.2 ms |
| 3.0 million/s | 2.44, 2.55, 2.32 million/s | 22.9, 35.7, 23.1 ms |
| 4.0 million/s | 2.49, 2.66, 1.52 million/s | 32.1, 47.2, 123.2 ms |
| 6.0 million/s | 1.80, 2.51, 2.51 million/s | 114.3, 63.7, 51.1 ms |

- The knee is between 1.6 and 2.0 million requests per second in total for this order stream: at 1.6 million one of three runs already fell behind (total p50 5.6 ms); from 2.0 million every run did. Above the knee the completed rate was 1.5 to 2.7 million per second (most runs 1.8 to 2.7).
- Above the knee latency is backlog: offered rate exceeds the completed rate, so the delay grows with the length of the run (these runs last 0.23 to 0.4 s). It is not the latency of a system at a steady load.
- Engine p50 stays 220 to 390 ns at every rate, so the time to apply one request did not grow with load.
- Ring-full retries were 0 in every run because the producers stop at 1,023 requests without a reply first (hundreds of millions of wait-loop iterations, which count loop passes and not events).
- It is not known whether the limit is the sequencer thread or the two producer threads (which also build requests and read replies), so this is the capacity of this bench on this machine, not of the sequencer. The completed rate (at most 2.66 million/s) is below 1 / (engine p50), which says the sequencer does more per request than apply; how much is not measured. One run at 4.0 million/s completed only 1.52 million/s with engine p50 387 ns: an unexplained disturbance, kept in the table.

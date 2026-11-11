# Open questions (not explained, not claimed)

Each item is something I saw and cannot yet explain. None of these is a result.

1. SPSC queue throughput is bimodal: 17-30 M items/s in 7 runs and 175-325 M items/s in 9 runs, nothing between (009). Failed push/pop counters did not separate the modes.
2. A cache-miss-like tail of about 450 ns appears at p99.9 in book benchmarks (008). Possible cause: WSL2 nested paging. Not tested.
3. Millisecond-scale stalls (max latency of 1 to 15 ms) show up in single runs in 010, 012, 013 and in the live view. Cause unknown.
4. p99 at 4 M orders/s is higher than p99 at 12 M orders/s in non-overloaded runs (010).
5. `fdatasync` under WSL2 returns in 0.4 us (012). Durability claims need bare-metal Linux.
6. CRC in journal append is bytewise and probably costs more than the book operation itself (011). Not profiled.
7. Pipeline numbers are burst measurements (400k orders per rate). No sustained-throughput measurement yet.
8. "No heap allocation on the hot path" is intended and not yet proved by a test.
9. Replication latency breakdown (10 to 18 us p50 for gated mode) is not split into its parts (013).
10. In the live view, p50 fell from 83 ns to 30 ns within a 5-second run while the book stayed small. Cause unknown (frequency, WSL2, or cache warm-up).
11. Raft: the figure-8 mutant is found only when entries trickle out one at a time (015). Why exactly is my explanation, not tested.
12. Kraken v2 WebSocket connection dropped 3 times in one 30 s recording (ConnectionClosedError: no close frame; ConnectionResetError). Cause unknown: Kraken, WSL2 network, or the client's ping setting (ping_interval=20). Not tested.
13. The Kraken checksum covers the top 10 levels only; the book was subscribed at depth 25. Levels 11 to 25 are not verified by it.
14. Recording gaps: three gaps of 10.2 h, 7.9 h and 35 min in the 6 to 7 Nov files line up with Windows sleep events (Kernel-Power 42 and 107). The recording that stopped on 7 Nov around 12:30 UTC has no sleep event; the laptop was probably shut down (not checked). Recording only runs while the laptop is on.
15. Reconnects: 130 snapshots in 8.47 hours of recording; median segment 20.3 s and 73 of 130 shorter than 60 s. Cause unknown (see 12).
16. Spread: in the longest segments 2 to 4 percent of book messages show a spread above 1 USD (maximum 39.4 USD), and the p99 spread of 7.2 hours is 57 ticks. The rebuilt book matched Kraken's checksum, so these are states of the feed. Why they occur is not known.
17. Trade signs: 85.3 percent of trades have the same side as the previous trade (31,793 trades). Hypothesis, untested: one aggressive order printed as several trades.
18. OMS memory: `seen_` (every request with its result) and `orders_` (every order) are never erased, and `open_orders()` scans all order ids ever. Found by reading the code; memory growth and speed were not measured. Hypothesis, untested: the OMS path, not the book, sets the sequencer's per-request cost.
19. Double apply is invisible: the OMS answers a repeated (account, cl_ord_id) from its first result and a repeated cancel with NotOpen, so a sequencer bug that applies a Submit or Cancel twice leaves state, digest and replay unchanged (mutant ApplyTwice, test_sequencer_mutants). This is by design for retries, but it means replay cannot detect that kind of bug. Detecting it needs a counter, such as applied count against sequence number. Not built.

Update to doubt 18 (OMS unbounded memory): measured in bench/oms_alloc_bench.cpp, 3 runs on WSL2: 2.008 allocations and 193.8 live bytes per request, identical each run; the book allocates nothing. Which allocations these are is still a hypothesis.

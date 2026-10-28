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

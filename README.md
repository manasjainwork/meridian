# Meridian by MANAS JAIN

A matching engine in C++20, built the way a trading firm would build one: every component has a design doc, a deterministic test, a fuzzer or a simulator, and a write-up that separates what was measured from what is only a hypothesis.

Status: work in progress (month 1 of a 12-month plan). Everything here is free to build and run; no paid services.

## Architecture

```mermaid
flowchart LR
    C[Order client] -- TCP binary protocol --> G[Gateway]
    G --> Q1[SPSC queue]
    Q1 --> B[Order book<br/>price-time priority]
    B --> Q2[SPSC queue]
    Q2 --> J[Journal writer thread<br/>CRC32, seq numbers]
    J --> R[Replicator]
    R -- TCP --> F[Follower<br/>apply + ack]
    J --> P[Replay / recovery]
    subgraph Consensus
      N1[Raft node 1] --- N2[Raft node 2] --- N3[Raft node 3]
    end
    R -. future: replicate through Raft log .-> Consensus
```

| Component | What it is | Header / source |
|---|---|---|
| Wire protocol | Fixed binary messages, explicit endianness | `include/meridian/protocol.hpp` |
| Order book | Fixed pool, intrusive FIFO per level, open-addressing id map, 64-ary bitmap for best price | `order_book.hpp`, `price_bitmap.hpp` |
| SPSC queue | Lock-free ring, acquire/release, cached opposite index | `spsc_queue.hpp` |
| Journal and replay | 32-byte records, CRC32, sequence numbers, one shared `apply()` | `journal.hpp`, `replay.hpp` |
| Journal writer | Dedicated thread, group commit | `journal_writer.hpp` |
| Replication | Leader/follower, batched cumulative acks, ack-before-apply | `replication.hpp` |
| Raft election | Pure state machine plus a seeded cluster simulator | `raft_node.hpp`, `raft_sim.hpp` |

## Design decisions and tradeoffs

1. **Bitmap for best price instead of a scan or `std::map`.** A 64-ary bitmap finds the best price in a few word operations. Cost: memory proportional to the price range and a bounded tick grid. See `docs/bugs/007`.
2. **Fixed pool and intrusive lists, no heap on the book's hot path (intent).** Predictable latency, cache-friendly. Cost: a hard capacity limit. The "no allocation" claim is not yet proven by a test.
3. **SPSC queue instead of a mutex queue.** Compared in `docs/bugs/009`. Cost: exactly one producer and one consumer; fan-in needs a different design.
4. **Event sourcing with one shared `apply()`.** Live path and replay path run the same code, and an order-sensitive digest detects divergence. Cost: the journal format becomes a compatibility surface.
5. **Replication acks before apply and never beyond the first invalid record.** Cost: a follower can lag the leader's applied state. See `docs/bugs/013`.
6. **Raft as a pure state machine.** No clock, sockets or threads inside the node, so a deterministic simulator can inject loss, duplication, reordering, crashes and partitions from one seed. Cost: an I/O layer must be written later.
7. **Open-loop load generation.** Latency is measured from the intended send time to avoid coordinated omission.

## Testing approach

1. Unit tests for every component, including endianness and protocol edge cases.
2. Differential fuzzing of the order book against a `std::map` reference, with invariant checks.
3. ASan, UBSan and TSan runs.
4. Journal truncation, corruption and gap detection tests.
5. Raft: invariants checked after every simulated event, thousands of chaos seeds, liveness after healing, determinism, and mutation tests (five injected bugs, all caught).
6. A claim counts only after at least 3 runs. Anything else is labelled a hypothesis. See `docs/methodology.md`.

## Measurements

Environment: WSL2, i5-11400H, TSC 2.688 GHz, open-loop load, at least 3 runs per row. These numbers compare variants on one machine. They are **not** absolute latency or durability claims; bare-metal Linux is still to do.

| What | Result | Write-up |
|---|---|---|
| Order book op (single thread, 400k ops) | p50 23-39 ns; p99 5-7x the median; p99.9 about 20x | [006](docs/bugs/006-order-book-baseline.md) |
| Best-price lookup, sparse book | bitmap p50 20.1 ns / p99 21.6 ns vs linear scan p50 26,478 ns / p99 57,844 ns | [007](docs/bugs/007-bitmap-best-price.md) |
| SPSC vs mutex queue, round trip | mutex/SPSC about 4-5x at p50, 7-16x at p99; SPSC p99.9 about 1.6-1.7 us, cause unknown | [009](docs/bugs/009-spsc-vs-mutex.md) |
| SPSC throughput | bimodal: ~17-30 M items/s in 7 runs, ~175-325 M items/s in 9 runs; no single number claimed | [009](docs/bugs/009-spsc-vs-mutex.md) |
| Pipeline (client, book, ack), 0.5-8 M orders/s | p50 230-320 ns; p99 6-20 us at 2 M/s, 34-52 us at 4 M/s, 60-137 us at 8 M/s | [010](docs/bugs/010-pipeline-baseline.md) |
| Pipeline saturation | between 12 and 16 M orders/s (burst of 400k orders, exact point not established) | [010](docs/bugs/010-pipeline-baseline.md) |
| Journal append | p50 59-65 ns, p99 65-387 ns, p99.9 1.2-1.35 us; costs more than the book op itself | [011](docs/bugs/011-journal-replay.md) |
| Journal in pipeline, p50 | none 221-419 ns, async 252-513 ns, gated 722-2240 ns; p99 noisy and inconclusive | [012](docs/bugs/012-journal-writer-pipeline.md) |
| Replication, p50 | none 201-355 ns, async 207-398 ns, gated 9.1-29.1 us (typically 10-18 us) | [013](docs/bugs/013-journal-replication.md) |
| Replication correctness | 48 of 48 replicated runs: replica digest equals the single-thread reference | [013](docs/bugs/013-journal-replication.md) |
| Raft election | 3600 chaos seeds, 0 safety violations; 5 of 5 injected bugs caught | [014](docs/bugs/014-raft-election-simulation.md) |

Caveats that matter:
1. `fdatasync` on WSL2 has p50 0.4 us, so it is effectively a no-op there. The "gated" modes measure the software path, not durable storage ([012](docs/bugs/012-journal-writer-pipeline.md)).
2. Each pipeline rate sends only 400k orders, so these are burst measurements, not sustained throughput.
3. Single runs showed outliers of milliseconds; the tail is not yet explained.

## Not established yet

1. Bimodal SPSC throughput: unexplained.
2. Cause of the ~450 ns tail (likely cache-missing loads, possibly WSL2 nested paging): unverified.
3. Millisecond-scale stalls: unexplained.
4. Durability and tuning claims: need bare-metal Linux.
5. Allocation-free hot path: no proof test yet.
6. Sustained (not burst) throughput: not measured.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer example:

```bash
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Iinclude -o /tmp/t tests/test_raft_election.cpp && /tmp/t
```

## Roadmap

1. Raft log replication and commit, then pre-vote and CheckQuorum
2. Order types (IOC, FOK, stop), modify
3. Snapshots and crash recovery
4. UDP market-data publisher and gateway hardening
5. Risk checks and order management
6. Profiling with `perf`, flamegraphs and cachegrind, with a measured speedup
7. Market-maker bot and backtester
8. Latency dashboard
9. Bare-metal Linux re-measurement

## Write-ups

Every experiment, bug and correction is in `docs/bugs/` (001 to 014), including the ones where an earlier claim turned out to be wrong.

# Why these methods, their cost, and how they would scale

Everything below comes from reading the code (include/meridian/order_book.hpp, price_bitmap.hpp, oms.hpp, sequencer.hpp) and from sizeof checks. Nothing here is a
new measurement. Where an earlier measurement exists it is cited, and it was taken on WSL2, so it is an observation and not a benchmark. Scaling paths are designs,
not built and not measured.

Symbols: P = number of price ticks (max_price), N = order pool size (max_orders), F = fills produced by one order, K = price levels that one order empties,
L = bitmap levels = ceil(log64 P) (3 levels cover 262,144 prices).

## Why these methods
The detailed list with costs is in the README section "Design decisions and tradeoffs". In one line each:
- **Bitmap for the best price:** finds the next non-empty level in L word probes instead of scanning P levels or walking a tree. Cost: memory proportional to P, a bounded tick grid.
- **Fixed pool and intrusive lists:** no allocation inside the book, FIFO per price level, stable memory. Cost: a hard capacity (PoolFull). The "no allocation" claim has no proving test yet.
- **SPSC rings:** no lock on the hot path. Cost: one producer and one consumer per ring, so fan-in needs one ring per producer (sequencer design D2).
- **Event sourcing with one shared apply():** live path and replay run the same code, so replay can prove a run was reproduced. Cost: the log format becomes a compatibility surface.
- **Raft as a pure state machine:** a deterministic simulator can inject loss, crashes and partitions from one seed. Cost: an I/O layer is written separately.
- **Single sequencer thread:** a recorded order and a measurable per-order delay, which the research needs. Cost: one writer, so throughput is bounded by one core.
- **Measurement rules** (docs/methodology.md): open-loop latency, p50 to max never the mean, at least 3 runs, WSL2 numbers are not benchmarks.

## Time cost (from the code)
| Operation | Time | Notes |
|---|---|---|
| Best bid or ask | O(1) | cached in best_bid_ and best_ask_ |
| Add that does not cross | O(1) expected + O(L) | hash lookup, FIFO append, bitmap set |
| Add that matches | O(F + K*L) | each fill takes the head of a FIFO; each emptied level costs a bitmap clear and a next-level search |
| IOC or market order | same as above | the remainder is cancelled, not rested |
| FOK check | O(K'*L) | walks the crossing levels, changes nothing |
| Cancel | O(1) expected, +O(L) if the best level empties | unlink from the FIFO, erase from the id map |
| Modify, same price and not larger | O(1) | keeps its place in the queue |
| Modify, any other change | cancel + add | loses time priority |
| check_invariants | O(P + N) | tests and debug builds only |

"Expected" means open addressing with at most half the slots used (the table has at least 2N slots). Long probe clusters would cost O(N) in the worst case; this was
not tested with adversarial ids.

## Space cost
| Part | Size | Default config (N = 65,536, P = 1,000) |
|---|---|---|
| Order pool | N * 32 B | 2,097,152 B |
| Price levels (bids and asks) | 2 * P * 16 B | 32,000 B |
| Id map (keys 8 B + values 4 B per slot, at least 2N slots) | >= 2N * 12 B | 1,572,864 B |
| Bitmap | about P / 8 B | a few hundred bytes |
| Total | | about 3.7 MB |

The sizes of Order (32 B) and Level (16 B) were checked with sizeof. All of this is allocated once in the constructor.

## The OMS layer (after the three changes in docs/bugs/028)
The book is bounded. The OMS around it was not; three changes bounded most of it.
| Structure | Type | Cost | Bound |
|---|---|---|---|
| orders_ | OrderStore: chunks of 1024 ids, plus a small map for old orders that are still open or pending | O(1) | about max_orders recent ids plus every open or pending order; older finished orders are forgotten |
| seen_ | ring of the last seen_window requests with a chained hash on two flat arrays | O(1) expected | seen_window requests (default 65,536); an older duplicate is treated as new |
| Result.fills | up to 4 fills inside the Result, a vector beyond that | O(F) | no allocation for 4 fills or fewer |
| accounts_ | std::map | O(log A) | grows with the number of accounts |
| pending stops | std::set | O(log S) | grows with waiting stop orders; allocates |
| open_orders(account), digest(), check() | walk of the kept records | O(kept) | for state comparison and queries, not the hot path |
| would_self_trade() | walk of the kept records when the account has resting orders on the other side | O(kept) | still a scan |

Measured (docs/bugs/028, same deterministic 200,000 operation stream, WSL2): allocations per operation 2.008 before, 0.030 after; live bytes growth per operation 193.8 before, 46.3 after. The bare book makes 0 allocations in the same stream. The remaining growth is order chunks that are still inside the kept window in that benchmark; the memory bound itself is shown only by a functional test (test_oms_retire), not by a memory measurement.

Behaviour changes that came with it (doubts 20 and 21): an old duplicate is accepted as new, and a finished order older than the window is unknown to order() and cancel().

Still hypotheses or open: where the last 5,821 allocations of trading orders come from; whether the OMS path really sets the sequencer's per-request cost (no profile with perf yet); the right size for the idempotency window.

## What exists as measurement
- SPSC ring against a mutex queue: about 4 to 5 times faster at p50 in ping-pong, throughput bimodal with no known cause (docs/bugs/009, WSL2).
- Book pipeline (ingress, ring, book, ring, egress): p50 about 250 ns up to 8 M orders/s, overloaded runs between 12 and 16 M orders/s, burst test of 400k orders per rate (docs/bugs/010, WSL2).
- OMS path: allocations and memory growth per request before and after three changes (docs/bugs/028, WSL2). Its timing was noisy and no timing claim is made. Nothing yet for the sequencer under load, or replicated orders at scale.

## How it could scale (designs, not built)
1. **Bigger price range.** Memory grows linearly with P; bitmap levels grow with log64 P, so L stays 3 up to 262,144 prices. The pool N is a hard limit: a full pool rejects with PoolFull.
2. **Bounded OMS memory.** Done in part (docs/bugs/028): idempotency window and order chunks. Open: pending stops, accounts, a memory measurement of the bound, and the right window size.
3. **More throughput.** One writer thread per book is by design (D1). The pipeline split in doc 010 already separates ingress, engine and egress threads. The OMS no longer allocates on most requests (docs/bugs/028); a profile with perf is the next step.
4. **More instruments.** One book and one sequencer thread per instrument (shard by instrument id). Cross-instrument risk, such as a position limit across instruments, needs a cross-shard rule that does not exist yet.
5. **More producers.** One SPSC ring per producer. The sequencer polls all rings each pass, so a pass costs O(number of producers) even when idle. An epoll based I/O thread (D9) is the later change for many connections.
6. **Replication.** A Raft group per shard. Textbook Raft sends one AppendEntries per follower per batch. The log grows without snapshots, and a restarted node replays the whole log; snapshots and log compaction are next on the roadmap.
7. **Bare metal.** Every latency and durability claim above waits for bare-metal Linux.

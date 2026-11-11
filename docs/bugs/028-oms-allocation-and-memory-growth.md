# OMS allocates on every request and its memory grows without bound

Status: measured on one machine; cause of the two allocations not yet identified.

## What was measured
bench/oms_alloc_bench.cpp runs the same deterministic stream (200,000 operations, 70% limit adds, 30% cancels, seed 12345) through the bare OrderBook and through the Oms. Single thread, TSC timing, counting operator new. WSL2 on a laptop, so these are observations, not a benchmark. Three runs.

## Results (3 runs)
- Book: 0 allocations, 0 bytes of live growth. p50 47 / 67 / 57 ns. p99.9 546-577 ns. max 36-136 us.
- Oms: 401,696 allocations (2.008 per op) and 38,765,456 live bytes of growth (193.8 per op), identical in all three runs. p50 304 / 351 / 339 ns. p99.9 6.5-29.8 us. max 4.4-5.1 ms.
- Oms invariant check passed in every run.

## What this supports
- The book path does not allocate; the Oms path does.
- Oms memory grows by about 194 bytes per request and nothing frees it (consistent with the unbounded seen_ and orders_ described in complexity-and-scaling.md).
- Oms service time is several times the book's at p50 and much worse in the tail.

## Not shown
- Which two allocations per op these are. Hypothesis: one node in seen_ and one in orders_. Not verified.
- Whether the tail comes from allocation. The max of several milliseconds may include page faults or OS scheduling on WSL2.
- Any bare-metal number.

## Next
Bound seen_ and orders_, remove the allocations, rerun this benchmark three times, compare.

## Step 1: bounded idempotency memory (measured, 3 runs, WSL2)
Change: seen_ (std::map, never erased) became a fixed ring of the last seen_window requests (default 65,536) with a chained hash over two flat arrays; orders_ now reserves max_orders buckets up front. Tests oms, raft_oms, sequencer and sequencer_mutants pass.

Same benchmark, before -> after:
- allocations per op: 2.008 -> 1.256 (401,696 -> 251,225), identical in all 3 runs
- live bytes growth per op: 193.8 -> 66.8 (38,765,456 -> 13,350,912), identical in all 3 runs
- p50: 304 / 351 / 339 ns -> 181 / 202 / 201 ns
- p99.9: 6.5-29.8 us -> 2.2-16.1 us
- max: 4.4-5.1 ms -> 48-171 us

Still not shown: the cause of the old multi-millisecond max (hypothesis: rehash of orders_, not verified), and how much of the remaining growth is the ring filling up once versus orders_ nodes.

Behaviour change: a duplicate cl_ord_id older than seen_window requests is treated as a new order. Recorded as doubt 20.

## Remaining
orders_ nodes (never erased), fills vector copies in Result, open_orders() and would_self_trade() scan all orders ever seen, check() and digest() walk all history.

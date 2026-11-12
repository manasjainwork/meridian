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

## Step 1: bounded idempotency memory (measured, 6 runs in two sessions, WSL2)
Change: seen_ (std::map, never erased) became a fixed ring of the last seen_window requests (default 65,536) with a chained hash over two flat arrays; orders_ now reserves max_orders buckets up front. Tests oms, raft_oms, sequencer and sequencer_mutants pass.

Same benchmark, before -> after:
- allocations per op: 2.008 -> 1.256 (401,696 -> 251,225), identical in all 6 runs
- live bytes growth per op: 193.8 -> 66.8 (38,765,456 -> 13,350,912), identical in all 6 runs
- p50: 304 / 351 / 339 ns -> 176-202 ns (6 runs)
- p99.9: 6.5-29.8 us -> 2.2-16.1 us (6 runs; wide spread, WSL2 noise)
- max: 4.4-5.1 ms -> 47-171 us (6 runs)

Still not shown: the cause of the old multi-millisecond max (hypothesis: rehash of orders_, not verified), and how much of the remaining growth is the ring filling up once versus orders_ nodes.

Behaviour change: a duplicate cl_ord_id older than seen_window requests is treated as a new order. Recorded as doubt 20.

## Remaining
orders_ nodes (never erased), fills vector copies in Result, open_orders() and would_self_trade() scan all orders ever seen, check() and digest() walk all history.

## Step 2: order records in fixed chunks (measured, 3 runs, WSL2)
Change: orders_ (unordered_map, one node per order, never erased) became OrderStore: records in fixed chunks of 1024 ids. A chunk older than the kept window (default max_orders ids) is freed; its open and pending orders move to a small map, finished ones are forgotten. New test tests/test_oms_retire.cpp (200,000 orders through an Oms with max_orders 4096): the old open order is still found and can be cancelled, an old finished order is forgotten (order() returns nullptr, cancel gives UnknownOrder), check() passes. All 20 tests pass.

Same benchmark, step 1 -> step 2:
- allocations per op: 1.256 -> 0.556 (251,225 -> 111,148), identical in all 3 runs
- live bytes growth per op: 66.8 -> 56.3 (13,350,912 -> 11,252,512), identical in all 3 runs
- p50 157 / 202 / 171 ns, p99 655-975 ns, p99.9 5.3-15.3 us, max 82-117 us. Step 1 was p50 176-202 ns. No clear time change is claimed; the spread between runs is as large as the difference.

What this does not show: the memory bound. The benchmark places 140k orders and the kept window here is 262,144 ids, so nothing is freed in it and the live growth is the chunks. The bound is only shown functionally by test_oms_retire, not by a memory measurement.

Behaviour changes: order() and cancel() on a finished order older than the window answer "unknown"; digest() no longer includes forgotten finished orders (two Oms with the same history still agree). Recorded as doubt 21.

## Remaining
The 111,148 allocations left are most likely the fills vector copied into Result for orders that trade (hypothesis, not verified). pending_ is a std::set and still allocates per stop order. would_self_trade() and check() still scan every kept record.

## Where the remaining allocations are (measured, 3 runs, WSL2)
The benchmark now counts allocations separately for orders that traded and orders that did not. Identical in all 3 runs: 60,626 orders that traded made 111,062 allocations (1.83 per traded order); the other 79,588 orders made 86 in total. So the allocations left after step 2 come from orders that trade. Not yet shown which allocations inside a trading order: the fills vector copied into Result is the likely one, and the first fill-carrying entries written into the idempotency ring may be part of it.
Timing in these three runs was noisy (p50 180-327 ns), so no timing claim is made from them.

# 016 — Order types (IOC, FOK, market) and modify

## 1. What was added
1. `OrderBook::submit(id, side, tif, price, qty, out)` returning an `ExecReport {status, filled, rested, cancelled}`. `add_limit` is now a wrapper for `submit(..., GTC, ...)`.
2. Time in force: GTC (rest the remainder), IOC (cancel the remainder), FOK (all or nothing; decided by walking the opposite levels before anything changes).
3. `submit_market`: an IOC at the extreme price. It never rests.
4. `modify(id, new_price, new_qty)`: same price and a smaller or equal quantity keeps the queue position; anything else loses time priority and re-enters as a new order, trading first if it crosses the book.
5. `tests/test_order_types.cpp`: 11 hand-written scenarios, a differential fuzz against a `std::map` reference, and properties checked after every operation.

## 2. Results (reproduced on my machine)
1. The order-types test passes in 3 runs, in the linear-scan build (`MERIDIAN_BOOK_LINEAR_SCAN`), and under ASan/UBSan. Every run prints `fuzz: 240000 operations, 2460 rejected, 32123 accepted modifies, FOK 4963 filled / 24084 killed`.
2. The fuzz compares every result with the reference: reports, fills in order, best bid and ask, order count, and per-level quantity at the end of each round. It also checks `check_invariants()` after every operation.
3. Properties checked on every accepted order: filled + rested + cancelled equals the quantity; the sum of fills equals `filled`; no fill is worse than the limit price; fills walk the book outward; only GTC rests; FOK is all or nothing; total resting quantity changes by exactly `rested - filled`.
4. All 11 earlier tests still pass with the new book (`100% tests passed, 0 tests failed out of 11` before this test was added).
5. Matching result unchanged: `book_bench` prints `fills=198016` and `checksum 34382` in every run, old and new.
6. Speed, old header against new header, alternating in one session, 3 pairs: new p50 is equal or lower in 9 of 9 comparisons (rest 23.1 / 23.8 / 27.5 vs 27.5 / 25.3 / 28.3 ns; match 36.5 / 39.4 / 45.4 vs 45.4 / 40.9 / 47.6 ns; cancel 20.1 / 20.8 / 24.6 vs 23.8 / 21.6 / 24.6 ns). Both versions got slower together in run 3.

## 3. Not established
1. "The new book is faster": not shown. The differences are at most 9 ns, shrink when the machine is slower, and could be noise or code layout. The only claim is that no slowdown was seen.
2. p99, p99.9 and max in the A/B runs vary by 2x between runs for the same binary (WSL2 noise). No claim about the tail.
3. The tests were run only through the public API. I did not test mutated versions of the book on my machine, so I make no claim about how sensitive the tests are to bugs.
4. Stop orders and stop-limit orders are not implemented.
5. Self-trade prevention, price bands, minimum tick and lot sizes, and per-side order limits are not implemented.
6. Modify is cancel/replace semantics. There is no "modify and keep priority on a quantity increase" mode.
7. A FOK order walks levels with a loop. Its cost grows with the number of crossed levels; I have not measured it.
8. No benchmark of IOC, FOK, market or modify paths yet; `book_bench` still measures only GTC rest, match and cancel.

## 4. Next
1. Stop orders; self-trade prevention.
2. Benchmark the new order types.
3. Show the new order types in the demo.

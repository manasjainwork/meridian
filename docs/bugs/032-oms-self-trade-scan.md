# 032: The OMS self-trade check scanned every order record

## How it was found
The first open-loop run of bench/seq_latency (M4) did not finish in the time expected. A single-thread program (bench/oms_scale.cpp) with the same order stream, timed in blocks of 40,000 requests, showed that the cost of a request grew with the number of orders.

## Cause
Oms::would_self_trade ran orders_.all(...) over every order record whenever the account had any open order on the other side. The cost of one request therefore grew with the number of order records kept (up to max_orders), not with anything the request touched.

## Fix
Each Account keeps buy_lv and sell_lv: open lots by price, ascending, only prices with lots above zero, updated in add_open at every place an open order changes (rest, partial or full fill of a maker, cancel, modify). The check looks at the best opposite price (first or last entry). The old scan stays as would_self_trade_scan and runs beside the index only when Config::crosscheck_self_trade is set (tests). Oms::check() rebuilds the index from the order records and compares it.

## Measured (author machine, WSL2, oms_scale with one account, 3 runs each, time for each block of 40,000 requests)
- Before: first block 356 to 402 ms, last block (requests 160,000 to 200,000) 4.7 to 5.1 seconds.
- After: first block 4.3 to 4.6 ms, last block 6.6 to 9.0 ms.
- test_oms_selftrade_index: 160,000 requests (4 accounts, limit, market, IOC, FOK, stop, stop-limit, cancel, modify), 49,571 self-trade rejects, 0 disagreements with the scan, check() every 200 requests; ok in 3 runs. ctest: 25 of 25.

## Not shown
- In the assistant's sandbox only (not on the author machine): 6 of 6 deliberately broken versions of the index were caught by the new test; allocations in oms_alloc_bench went from 2,301 to 2,313 in 200,000 requests.
- Insert and erase in the per-account vectors cost time proportional to the number of distinct prices of that account; with thousands of distinct prices per account this would show. Not measured.
- No other scan in the OMS was searched for beyond this one.

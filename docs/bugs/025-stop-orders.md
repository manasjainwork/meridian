# 025: stop and stop-limit orders

## What was missing
No way to say "sell if the price falls to X". A stop order waits outside the book and fires when the last trade price reaches its stop price.

## What was built
- `Type::Stop` (becomes a market order) and `Type::StopLimit` (becomes a limit order). Status `Pending` while waiting, `Triggered` after it fires.
- Buy stop fires when last trade price >= stop price, sell stop when <=. Several stops fire lowest order id first, and trades from a fired stop can fire more.
- A stop that is already past its price when submitted fires at once.
- At the trigger the order goes through the normal checks (position limit, self-trade). If refused it becomes Cancelled, it is not stuck.
- Entry checks size and position limit only; pending stops do not count toward exposure until they fire.
- Pending stops can be cancelled; modify of a pending stop is refused (not_open). A retry with the same client order id returns the same stop.
- Gateway: `type` stop or stop_limit with `stop_price`; `/account` lists waiting `stops`; `triggered_fills` count in the order answer; fills of fired stops appear on the trade tape.
- Trade tab: Stop and Stop-limit order types and a "Stop orders waiting" panel.

## What was measured (one run, my machine)
- `build/test_oms`: fuzz 120 seeds x 1200 operations, 0 problems; the invariant check fails if a pending stop should have fired. Mutants "stop orders never fire" and "stop orders fire in the wrong direction": caught in 40 of 40 seeds each; the five older mutants still 40 of 40.
- `tools/gateway_test.py`: ALL PASS. With `--bug 32` it reports 3 FAILED, with `--bug 64` 5 FAILED.
- TypeScript check passes.
Single runs; not a claim until repeated 3 times.

## Not done / unknown
- Not driven by hand in the browser yet on my machine.
- Triggering scans the pending set each time; fine at this size, an index by stop price is the later fix.
- No trailing stops, no modify of a pending stop, stops are not in the book mirror or snapshots.
- State is in memory only, not behind Raft yet.

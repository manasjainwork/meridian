# 022: order management (accounts, idempotent order ids, risk, positions)

## What was built
`include/meridian/oms.hpp`: a layer between the clients and the order book.
- Every request carries (account, cl_ord_id). The same request again returns the first answer with `duplicate = true` and changes nothing. The same key with different contents is rejected (KeyReuse). The purpose: a client that times out and retries cannot trade twice.
- Cancel and modify check that the order belongs to the account.
- Risk limits: maximum order size, and a position limit that counts the open orders on the same side (worst case, long or short).
- Positions and cash per account as exact integers (position in lots, cash in ticks times lots). Equity at a mark price is `cash + position * mark`.
- Deterministic: no clock, no randomness, ids from a counter, so the same request sequence gives the same state and the same digest. This is the property needed to put it behind the Raft log later.

## Tests (`tests/test_oms.cpp`), results from my machine, 3 runs, identical
- Directed cases pass (crossing trade at the resting price, exact position and cash, market, IOC, FOK, rejects, position limit with open orders, ownership, modify that keeps its place and one that crosses).
- Fuzz, 120 seeds of 1200 operations each (submits, retries, same key with different contents, cancels by any account, modifies), after every operation: book invariants, positions sum to zero, cash sums to zero, each account's open quantity equals its open orders, resting quantity in the book equals the open orders. 0 seeds with a problem.
- Same request sequence twice gives equal digests (5 seeds).
- Mutation tests, each in 40 seeds: retry executes again (no idempotency) caught 40 of 40; any account can cancel any order 40 of 40; open quantity not reduced on cancel 40 of 40; taker position and cash not updated 40 of 40.

## Limits (not established)
- Not thread-safe; the gateway must serialize calls.
- No self-trade prevention: one account can trade with itself (its position nets to zero). Planned.
- The idempotency table and the order table grow without bound. A real system keeps a window.
- A duplicate returns the answer to the original request (status at that time), not the order's current status.
- Cash and position use 64-bit integers; I have not analysed how much volume it takes to overflow them.
- Not yet behind Raft and not yet reachable over the network.

# 024: self-trade prevention (reject the incoming order)

## What was missing
An account could send a buy that matched its own resting sell. That produces a fill where maker and taker are the same
account: no real change of position, but it prints a trade, counts toward volume, and on a real exchange is a violation.

## What was built
The OMS now refuses an incoming order if it could match a resting order of the same account (`Reject::SelfTrade`,
gateway code `self_trade`, HTTP 422):
- buy limit at price P: refused if the account has an open sell at a price <= P
- sell limit at price P: refused if the account has an open buy at a price >= P
- market order: refused if the account has any open order on the other side
- modify: the same rule applies to the new price
A refused order changes no state. Mode is "reject incoming", not "cancel resting" and not "decrement and cancel".
Bug flag `kNoSelfTrade = 16` turns the check off for the mutation test.

## What was measured (one run, my machine)
- `build/test_oms`: directed self-trade cases pass; fuzz 120 seeds x 1200 operations, 0 seeds with a problem; the fuzz now
  also fails if any fill has the same account as maker and taker. Mutant "an account can trade with its own order":
  caught in 40 of 40 seeds. The four older mutants are still 40 of 40.
- `tools/gateway_test.py`: ALL PASS.
These are single runs on my machine. By my own rule they are not a claim until repeated 3 times.

## Not done / unknown
- The check scans all orders when the account has anything resting on the other side: O(orders). An index per account is the fix.
- The OMS test takes noticeably longer than before; I have not timed it on my machine or found which part is slow (hypothesis: the full invariant check after every fuzz operation).
- The other modes (cancel resting, cancel both, decrement) are not implemented.
- No gateway-level (HTTP) test for `self_trade` yet.

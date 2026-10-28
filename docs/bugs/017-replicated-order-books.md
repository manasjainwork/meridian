# 017: Replicated order books (Raft log applied to real books)

## What was built
`include/meridian/raft_apply.hpp`: a `Replica` (an order book plus trade and volume counters) with one deterministic `apply`, and a script of client requests (new order GTC/IOC/FOK, market, cancel, modify). A Raft entry's `cmd` is a request number; request k is `script[k-1]`; cmd 0 is Raft's no-op and is skipped.
`tests/test_raft_apply.cpp`: runs the 3-node simulator with loss, duplication, reordering, crashes and partitions, and checks the books.

## What the test checks
1. A node's book (its committed prefix applied) equals a reference book built from the simulator's own committed log up to the same index.
2. After healing, restarting all nodes and letting the cluster settle, every node's book equals the book of the whole committed log.
3. Every book passes `check_invariants`.
4. Two wrong ways of applying must be caught: applying the uncommitted tail of the log, and swapping the last two committed entries.

## Results (my machine, WSL2, 3 runs)
All three runs printed the same lines (the seeds are fixed, so this shows the test is deterministic, not that it was sampled three times):
- 300 seeds, 300 settled with identical books
- 19,704 orders applied in total, 8,562 trades, 732 prefix checks
- apply-uncommitted-tail mutant caught in 291 of 300 seeds
- swap-last-two mutant caught in 27 of 300 seeds
- apply cost alone (no Raft, no network), 400,000 requests, 9 measurements: 24.3 to 30.0 ns per request (includes decoding the request and counting fills)

## What this does not show
- It checks agreement of the log plus a deterministic apply. It does not test matching correctness (test_order_* does).
- About 66 orders are applied per seed, so logs are short.
- The swap mutant is caught in only 27 of 300 seeds. My explanation (neighbouring requests often do not change the book when swapped) is not tested.
- 24 to 30 ns/request is the cost of `apply` alone. It is not the latency of a replicated order (that includes network and disk) and says nothing about throughput through Raft.
- The entry holds a request number, not the order itself. A real system must put the order in the log entry.
- No snapshots: a restarted replica replays from index 1.
- All replicas run in one process in the simulator, not on separate machines.

# 026: replicated order management (state machine step)

## What was missing
Only the order book was replicated through Raft (doc 015/016). Accounts, positions, cash, idempotency and stop orders live in the OMS and were not.

## What was built
`include/meridian/raft_oms.hpp`: `OmsReplica` applies committed log entries to a full `Oms` (submit, cancel, modify) in index order. `tests/test_raft_oms.cpp` runs the Raft simulator with loss, duplication, reordering, crashes and partitions, 3 nodes, 300 seeds, and checks:
- each node's OMS equals a reference OMS built from the simulator's committed log up to the same commit index;
- after the network heals, every node's OMS is identical and equals the OMS of the whole committed log;
- every OMS passes its own full consistency check.
The script mixes limit, market, IOC, FOK and stop orders from 16 accounts, cancels, modifies, client retries and reused keys.

## What was measured (3 runs, my machine, identical output each time; the simulator is deterministic)
- 300 of 300 seeds settled with identical OMS state: 19,704 commands applied, 6,964 fills, 1,559 retries answered from memory, 732 prefix checks.
- Wrong ways of applying, caught in this many of 300 seeds: apply uncommitted tail 291, swap last two committed entries 136, one node with "taker not credited" 187, one node with "stops never fire" 186.

## Not done / unknown
- The request still comes from a shared script indexed by the log entry (the same as doc 016). The entry does not yet carry the request bytes, so a real client cannot use this path. That is the next step.
- The gateway is not connected to the cluster yet.
- The mutants are not caught in every seed (some seeds commit too little on node 0 for the bug to show); I have not checked how many of the escapes are that case.
- The OMS digest does not include the idempotency table itself; `OmsReplica::digest` adds the number of retries answered and refused requests as a proxy.
- No snapshots: a restarted replica replays the whole log.

# 015 — Raft log replication and commit on the deterministic simulator

## 1. What was added
1. `raft_node.hpp`: AppendEntries / AppendReply, the log-matching check on the previous entry, conflict handling, commit rule (current-term entries only), a no-op entry in every new leader's term, batching (`max_batch`), a back-off hint on refusal, `propose()`.
2. `raft_sim.hpp`: client commands, plus invariants checked independently of the nodes' own bookkeeping: State Machine Safety, Leader Completeness, Log Matching, commit durability (every committed entry is on a majority of disks), Leader Append-Only, commit never backwards or beyond the log, no command committed twice, no invented command.
3. `test_raft_replication.cpp`: unit tests on the node, scripted scenarios, chaos runs, liveness runs, determinism, mutation tests.
4. `test_raft_election.cpp` was adapted: `Heartbeat` became `AppendEntries`, and the "stale-log node never wins" check now stops once a leader has changed that node's log (afterwards it may legitimately win).

## 2. Results (reproduced on my machine: 3 identical runs, plus ASan/UBSan run of the replication test, 2m8s, no reports)
1. Chaos safety (n = 3, 5, 7; 3000 seeds; 15% drop, 10% dup, 1 to 100 ms delay, crashes, partitions, client commands): 0 violations. 187,082 commands proposed, 167,434 committed, 1,480 seeds-events where an existing log entry was removed or replaced, 2,988 of 3000 seeds committed something, 2,849,193 messages.
2. Same chaos with other message patterns (n = 5, 1000 seeds each): max_batch 1 / no-op on, max_batch 3 / no-op on, max_batch 64 / no-op off, max_batch 1 / no-op off: 0 violations in all four.
3. Liveness: after chaos stops and the network is healed, 450 of 450 seeds elect a stable leader, commit 20 new commands, and all live nodes end with an identical log and the same commit index.
4. Scripted scenarios pass: 100 commands on clusters of 1, 3, 5, 7 nodes committed in order; leader crash mid-stream keeps everything committed; a follower that missed 500 commands catches up through batched appends; a minority-partition leader's 10 uncommitted commands are overwritten after healing.
5. Determinism: same seed, same digest, message count and commit count (100 seeds); the 100 digests are all different.
6. Mutation tests, all five caught: commit old-term entries by counting replicas (Figure 8), append without the previous-entry check, truncate on every AppendEntries, commit with n/2 replicas, follower commit unbounded.
7. The election tests still pass on the new node. Their message count changed from 1,326,228 (election-only version, see 014) to 1,357,796, because the node now also sends a no-op and replication messages.

## 3. Not established
1. The Figure 8 mutant was caught only with `max_batch = 1` (first catch at chaos seed 2027269). With the default batch size and the no-op it was not found in 20,000 seeds in a side experiment. The reason (a no-op and a big batch travel together, so old-term entries cannot reach a majority alone) is my explanation, not something I tested directly.
2. Several mutants were caught through a simple symptom (commit index beyond the log, committed entry on too few disks) rather than through Log Matching or State Machine Safety. Those two checks have no mutant that is caught by them specifically.
3. The commit-durability check reads the simulator's copy of each node's persistent state, which the simulator takes from the node after every action. It does not model torn writes, partial disk failures, or a slow fsync.
4. Time is simulated milliseconds. Nothing here says anything about real network behaviour or real disk speed (fsync on WSL2 is not meaningful).
5. Liveness is shown only within a bounded number of simulated steps and with a healed, loss-free network.
6. Not implemented yet: snapshots and log compaction, membership changes, pre-vote, CheckQuorum, linearizable reads, client request deduplication. The known "isolated node runs its term away and deposes a healthy leader" weakness from 014 is still present.
7. The log is an in-memory vector of 16-byte entries. It is not yet written through the existing journal, so nothing here measures replication throughput or latency.
8. The sanitizer run was only done on the replication test, on one machine.

## 4. Next
1. Pre-vote and CheckQuorum (the characterisation test flips).
2. Snapshots and log compaction.
3. Connect the replicated log to the order book (apply committed entries through the shared `apply()`), then measure.

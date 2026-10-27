# 014 — Raft leader election on a deterministic simulator

## 1. What was built
1. `raft_node.hpp`: a Raft node as a pure state machine (no clock, sockets or threads). Election only: terms, votes, election restriction, heartbeats.
2. `raft_sim.hpp`: a seeded cluster simulator. Time, delay, loss, duplication, reordering, crash/restart and partitions are all driven by one seed, so a failing seed replays exactly.
3. `test_raft_election.cpp`: unit tests, scenario tests, chaos runs, liveness runs, determinism checks and mutation tests.

## 2. Invariants checked after every event
1. A node never votes for two candidates in one term, even across a crash (simulator keeps its own ledger).
2. At most one leader per term.
3. A leader had votes from a real majority according to that ledger.
4. A node's term never decreases.

## 3. Results (reproduced on my machine, 3 identical runs plus ASan/UBSan)
1. 3600 chaos seeds (n = 3, 5, 7; 15% drop, 10% dup, 1 to 100 ms delay, crashes, partitions): 0 safety violations; 1,326,228 messages delivered; max term 28.
2. 3596 of 3600 seeds elected at least one leader; 3469 saw more than one leader over time (so the runs exercise real re-elections).
3. After chaos stops and the network is healed, every one of 900 seeds converged to one stable leader.
4. Same seed gives the same digest and message count (200 seeds); the 200 digests are all different.
5. Election restriction: nodes with stale logs (shorter log, and longer log with an older last term) never became leader in 600 chaos runs; the cluster still elected someone else in 100 of 100 clean runs.
6. Mutation tests, all caught: vote twice (seed 5), forget vote on restart (seed 15843), quorum n/2 (seed 5), count duplicate votes (seed 5), skip log up-to-date check (stale node won).
7. Characterisation (one seed): an isolated follower's term ran from 1 to 16; on heal the healthy leader was deposed. This is the known weakness that pre-vote removes.

## 4. Not established
1. 4 of 3600 chaos seeds never elected a leader. Cause not examined. Hypothesis: crashes and partitions kept the cluster without a majority for the whole run. Unverified.
2. "Forget vote on restart" was caught only at seed 15843. This bug is rare under this fault mix; a different mix may miss it. Detection power per bug is not measured beyond first catch.
3. The simulator's ledger reads `persistent().voted_for` from the node, so it checks consistency of the node's reported vote, not an independent record of what was sent on the wire.
4. Time is simulated milliseconds. Nothing here says anything about real network latency, real timeouts, or real disk behaviour (fsync on WSL2 is not meaningful).
5. Persistence is modelled as "state saved after every node action". Torn writes and partial disk failures are not simulated.
6. No log replication, no commit, no membership change, no snapshots yet. Only elections are tested.
7. Liveness is shown only within a bounded number of simulated steps and with a healed, loss-free network.
8. Characterisation in item 3.7 is from a single seed, not a distribution.

## 5. Next
1. Log replication and commit rule, with a log-matching and leader-completeness checker in the simulator.
2. Pre-vote and CheckQuorum; the characterisation test flips.
3. Persistence of the real log through the existing journal.

# 019: Pre-vote and CheckQuorum

## Why
Two observations motivated this.
1. In 014 a node cut off from the cluster kept raising its term, and when it came back it deposed the healthy leader (a characterisation test, seed 41).
2. In 018 I started one node alone by hand. It went from term 1 to term 92 in about 20 seconds (one election timeout of 150 to 300 ms per term), and when the other nodes joined, the cluster had to adopt term 92. One run, my observation, from the terminal.

## What was built
- `Config::prevote` (default off): a node whose election timer fires first sends PreVote (term + 1, its last log entry) and changes nothing. Only if a majority would vote does it raise its term and run the real election. A receiver grants a pre-vote only if the proposed term is higher than its own, its log is not ahead of the candidate's, and it has not heard a valid AppendEntries from a leader within election_min_ms (a leader refuses too). Receiving a pre-vote changes no term, vote or timer.
- `Config::check_quorum` (default off): a leader that has not received an AppendReply from a majority (itself included) for election_max_ms steps down to follower without changing its term.
- Both are on in `raft_main` (real processes). Everything else, including the simulator tests of 014, 015 and 017, still runs with both off, so their numbers did not change.
- Two deliberate bugs: `kNoLeaderStickiness` (grant although a leader was just heard) and `kPreVoteAdoptsTerm` (granting a pre-vote changes the receiver's term).

## Results (my machine, WSL2; the simulator is deterministic, 3 runs printed identical lines)
`tests/test_raft_prevote.cpp`, 5 nodes, 40 seeds, each with one node cut off and with a 2-node minority cut off, 4000 ms, then healed:
- pre-vote off: healthy leader deposed in 80 of 80 runs, isolated node's term up by as much as 21
- pre-vote on: healthy leader deposed in 0 of 80 runs, no isolated node's term moved
- leader crashed, new leader elected again: 60 of 60 runs (3 and 5 nodes, 30 seeds each); a 1-node cluster elects itself; with 3 of 5 nodes down nobody becomes leader; with a majority back a leader appears
- CheckQuorum on: a leader cut off in a 2-of-5 minority stepped down in 30 of 30 runs, the slowest after 400 ms; off: it stayed leader for the whole 1500 ms in 30 of 30 runs
- healthy cluster, 10 s, perfect network, both on: the leader changed in 0 of 30 seeds
- chaos (loss 15%, duplication 10%, reordering, crashes, partitions, client commands), 150 seeds each: 0 safety violations with both on, for 3 and 5 nodes
- the two bugs were caught by the cut-off scenarios: 11 of 80 runs (no stickiness) and 40 of 80 runs (adopts term)
- `ctest`: 15 of 15 passed
- failover on real processes (018 script) with both on, 3 runs, all PASS: first leader after 258, 322, 316 ms; election after kill -9 of the leader 187, 226, 117 ms (018 without pre-vote: 147, 141, 189 ms; three runs each overlap, so I cannot say pre-vote made it slower or faster)
- one node started alone for 10 s on real processes (port 7600, no other node running), last STATE line: `STATE id=0 role=F term=0 leader=-1 log=0 commit=0 applied=0 orders=0 resting=0 trades=0 digest=a774cec6ac63ddb4`

## What this does not show
- Chaos commit counts differ between on and off (3 nodes: 9,328 on, 9,506 off; 5 nodes: 9,657 on, 8,477 off). These are single deterministic runs of one workload, not a performance comparison.
- The mutant `kNoLeaderStickiness` is caught in only 11 of 80 runs; the scenarios exercise the stickiness rule weakly.
- A restarted node remembers no leader contact, so it can grant a pre-vote right after a restart. Not tested.
- CheckQuorum on real processes was not tested (only in the simulator).
- No leadership transfer and no membership changes.
- Default is still off. Switching it on for everything needs the 014, 015 and 017 numbers to be regenerated.

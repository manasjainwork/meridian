# 018: Raft on real processes (UDP on localhost, state on disk)

## What was built
`src/raft_main.cpp`: one Raft node as an OS process. The `Node` class is the same pure state machine the simulator tests (014, 015). The process adds a clock, a UDP socket on 127.0.0.1 (port base+id), a state file, and a `Replica` (order book) that applies committed entries in order.
Rule kept from the design: the state file is written (temp file, `fdatasync`, rename) before any message produced by the same step is sent.
`scripts/raft_failover.sh`: starts 3 processes, waits for a leader, sends 200 order requests to it, `kill -9`s the leader, waits for a new leader, sends 200 more, restarts the killed node from its state file, and requires identical books on all 3.

## Results (my machine, WSL2, 3 runs, all PASS)
| run | first leader after start | election after kill -9 of the leader |
|-----|--------------------------|--------------------------------------|
| 1   | 319 ms                   | 147 ms                               |
| 2   | 318 ms                   | 141 ms                               |
| 3   | 232 ms                   | 189 ms                               |

In every run, at the end: all 3 nodes had commit = applied = log = 402 entries (400 orders plus a no-op per term), 400 orders applied, 217 trades, 31 resting orders, and the same digest `8d531db8584fdc3c`. After the first 200 orders all 3 had digest `201f237d9c904931`.
The order script is fixed (seed 1), so the digests are expected to repeat across runs. The varying numbers are the timings.

## How the times were measured
Election time = wall clock of the kill, to the wall-clock stamp (millisecond resolution) printed by the new leader when it became leader. Start time = from launching the three processes to the first `role=L` line seen by the script. Both include process scheduling noise of WSL2.

## What this does not show
- Only `kill -9` (process crash). Not power loss: `fdatasync` on WSL2 returns without a real flush (see doc 012), so durability needs bare-metal Linux.
- One machine, UDP over loopback: no real network loss, delay or partitions. Those were tested only in the simulator.
- 3 runs, one scenario. A different timing could expose a bug the script never hits.
- Election times of 141 to 189 ms sit inside the configured 150 to 300 ms timeout window measured from the last heartbeat. I have not measured where inside that window they fall.
- The state file is rewritten whole on every change (cost grows with the log), and there are no snapshots; a restarted node replays from index 1.
- Wire format is host byte order, no authentication, no encryption.
- Clients are not handled: a request sent to a non-leader is dropped with an EVENT line, nothing retries it.
- No pre-vote and no CheckQuorum yet.

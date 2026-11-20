# 034: M5 slice 1, virtual-clock sequencer

## What was built
`include/meridian/virtual_sequencer.hpp`: a single-threaded sequencer with no rings and no wall clock. Each message carries the virtual time its agent sent it and a one-way delay. Messages are applied in order of (arrival time, agent id, the agent's own message number). One agent's messages never overtake each other (a connection is FIFO). Sending into the past (arrival before the last applied message) is refused and counted.

## What the test shows (`tests/test_virtual_sequencer.cpp`, ctest `virtual_sequencer`)
- A hand-made case checks each rule: equal arrival goes to the lower agent id, same-agent FIFO, a smaller delay wins.
- 6 agents x 1500 messages with many equal times (more than 1000 adjacent ties in the applied order), pushed in 6 different cross-agent orders: the event log bytes, the OMS digest and the fills digest were identical every time.
- The event log replays (`evlog::replay`) to the same replies and digests. Virtual time never goes backwards.
- Four deliberately broken versions of the ordering rule (tie by insertion order, ignore delay, allow overtaking, highest agent id first) were each caught.
- Run 3 times on the author's machine, all passed. ctest 26 of 26.

## Not shown yet
- Latency injection from a measured or random distribution (hypothesis H4). Only fixed delays given by the caller were tested.
- Determinism across machines or compilers: only one machine was tested.
- Use by RL agents, and the cost of the std::map queue (not measured; fine for simulation, not meant for the real-time path).
- The real-time sequencer and the virtual one share `apply_msg`, but no test compares their logs on the same input.

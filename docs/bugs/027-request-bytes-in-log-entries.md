# 027: the request travels inside the Raft log entry

## What was missing
In docs 017 and 026 a log entry held only a request number. The request itself came from a script that every replica
shared, so a real client could not use the replicated path.

## What was built
- `Entry` (raft_node.hpp) has a new field `data`, opaque bytes. `Node::propose` takes optional bytes. Equality of entries
  includes `data`, so the Log Matching check in the simulator now also compares the request bytes.
- `Sim::payload` (raft_sim.hpp): an optional function that gives the bytes for command number `cmd`.
- `raft_oms.hpp`: `encode_cmd` / `decode_cmd`, a fixed 48-byte record in host byte order. `decode_cmd` refuses a wrong
  length and unknown enum values. `OmsReplica::apply_entry` decodes the request from the entry; bytes that do not decode
  are counted (`undecodable`, part of the digest) and skipped, the same way on every replica.
- `tests/test_raft_oms.cpp` now builds every OMS from log entries, not from the script. `cmd` stays a unique request number
  so the simulator can still check "never invented, never committed twice".

## What was measured (3 runs, my machine, identical output each time; the simulator is deterministic)
- Codec: 3000 of 3000 requests round-trip; all 48 truncations and 2 bad enum values are refused; applying requests through
  bytes gives the same digest as applying them directly.
- 300 of 300 seeds settled with identical OMS state: 19,704 commands, 6,964 fills, 1,559 retries answered from memory,
  732 prefix checks. These are the same numbers as in doc 026, so carrying the bytes did not change the result.
- Wrong ways of applying, caught in this many of 300 seeds: uncommitted tail 291, swap last two 136, taker not credited 187,
  stops never fire 186 (all same as 026), and the new one, one node applies its last committed entry with one request
  byte flipped, 140.
- `ctest`: 17 of 17 passed.

## Not done / unknown
- The real-process node (`raft_main.cpp`) writes only term and cmd on the UDP wire and in the state file. `data` does not
  travel there yet, so that node still takes requests from its script (doc 018).
- The gateway is not connected to the cluster. No client retry, no redirect to the leader.
- The record is 48 bytes in host byte order. Fine for replicas of one machine type, not a wire format.
- The flipped-byte mutant escapes in 160 of 300 seeds. I did not check why (hypothesis: some seeds commit too little on node 0
  for the flip to matter, or the flipped field does not change the OMS state).
- The other Raft tests (014, 015, 017, 019) passed under ctest after the change; I did not re-read their printed numbers here.

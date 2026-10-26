# 013: Journal replication to a follower (loopback TCP)

## Design
Leader engine copies each input to an SPSC queue; a replicator thread adds the CRC, batches records
(up to 4 KB) and sends them over TCP (TCP_NODELAY). The follower validates every record (CRC,
field ranges, seq == previous + 1), appends to its log, sends one cumulative ack per received batch,
then applies the batch to its own OrderBook. It never acks past the first invalid record.
Modes: none, async (acks not held), gated (ack released only after the follower's ack).
The follower is a thread in the same process on 127.0.0.1; its log is in memory.

## Tests (7, ASan/UBSan and TSan clean)
Fragmented stream (1-200 byte chunks) and big chunks: replica digest == leader digest, acked == N.
Flipped bit: BadCrc, valid prefix 5000, never acked beyond it, replica state == leader state at seq 5000.
Missing record: SeqGap, prefix 2999. Valid CRC but bad field: BadField. Early disconnect: Closed with
all received records acked. Record cut in half at disconnect: not applied, not acked.

## Results (WSL2, cpus 0/2/4/6/8, 400k orders, 6 runs per cell, ns)
- 48/48 replicated runs: replica digest == single-thread reference (db0a4bebee34aab4), all records received.
- repl-full spins: 0 in every run (no backpressure up to 4 M orders/s).
- p50: none 201-355; async 207-398; gated 9142-29102 (typically 10-18 us).
- gated p99 87-680 us (one run 4.0 ms); p99.9 208-700 us (one run 7.0 ms).
- Compare 012: fdatasync here is a no-op (~0.4 us), so the ~1 us "durable" latency measured there was not durable.
  The replicated ack costs ~10-18 us p50.

## Not established
- Where the ~10-18 us goes (send syscall, loopback stack, follower wake-up from a blocking recv,
  ack path, the replicator's polling): not profiled.
- Real network behaviour: same process, same host, loopback only.
- Failure behaviour: no leader election, no reconnect, no follower catch-up, single follower.
- Whether async replication raises tail latency (ms-scale outliers in 5/24 async runs vs 2/24 baseline; too few).
- Cause of the ms-scale outliers.

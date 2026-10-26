# 012: Journal writer thread inside the pipeline

## Design
Engine copies each input into a 64k-slot SPSC queue (no CRC, no I/O on the engine thread).
A writer thread computes the CRC, buffers, write()s, and fdatasyncs.
Modes: none (baseline), async (fdatasync every 10 ms, acks not held), gated (group commit:
fdatasync whenever the queue is empty; an ack is released only after its record's fdatasync).
Latency = intended send time -> ack released. Open loop, 400k orders, cpus 0/2/4/6, WSL2 ext4 (/tmp), 3 runs.

## Correctness
18/18 journaled runs: the file loads with status Ok, 400000 records, and replays to the same
digest (db0a4bebee34aab4) as a single-thread reference. `journal-full spins` was 0 in every run.

## Results (ns)
- p50: none 221-419, async 252-513, gated 722-2240 (rates 0.5-8 M orders/s).
- p99/p99.9: noisy in all modes (none and async both show 10 us - 5 ms outliers at 0.5 M/s).
  At 4-8 M/s async p99 was higher than none in 4 of 6 runs; not conclusive.

## Not established
- Durability of the gated mode on this machine: gated p50 of ~1 us is far below a real disk
  flush, so fdatasync under WSL2 may only reach the host cache. Needs a direct fdatasync
  measurement and, for a real claim, bare-metal Linux.
- Whether async journaling raises p99 (inconclusive).
- Behaviour above 8 M orders/s and for runs longer than 400k orders.

# 011: Event journal and deterministic replay

## Design
Every input event (add / cancel) is a 32-byte record: seq, id, price, qty, type, side, CRC32.
Replay feeds the records into a fresh OrderBook through the same `apply()` used by live
processing, and compares a 64-bit digest of every result code, every fill, and the final book state.
v1 scope: the journal lives in memory and `save()` writes it at the end. Not durable (no fsync),
not yet written by a separate thread, host byte order.

## Tests (8, ASan/UBSan clean)
save->load->replay equals live; replay twice equal; replay on a book with scrambled internal
layout (5000 orders added then cancelled first) equals live; CRC/seq valid on all 200k records;
300 random single-bit flips in the file: 0 undetected; truncation mid-record: detected and the
123456-record valid prefix replays to the same digest as that prefix run live; a deleted record
is caught as a sequence gap; one changed input (CRC fixed up) changes the digest.

## Measurements (WSL2, 3 runs, 400k events)
- Replay: 29-34 M events/s. Digest identical in all 9 replays.
- Digest identical to the one produced on a different machine (cloud Linux): db0a4bebee34aab4
  (same generator seed; both x86-64, g++ 13).
- Journal append: p50 59-65 ns, p99 65-387 ns, p99.9 1.2-1.35 us, max 50-105 us.
  The book op itself is p50 ~21-35 ns, so journaling currently costs more than matching.
- save() of 12.8 MB: 3.6-4.9 ms to the page cache. No fsync, so this says nothing about disk.

## Not established
- Durability / crash safety (no fsync, no writer thread).
- Determinism across CPU architectures or compilers.
- Journaling inside the multi-thread pipeline and its effect on end-to-end latency.
- Cost split inside append: CRC is bytewise (28 table lookups), suspected main cost, not profiled.

## Next
Move CRC off the hot path (compute in a writer thread) or use hardware CRC32C; add a journal
writer thread with real file I/O and fsync; re-run the pipeline benchmark with journaling on.

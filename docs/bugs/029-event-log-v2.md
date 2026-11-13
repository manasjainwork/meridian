# Event log v2 and replay (sequencer milestone M2)

Status: format, loader and replay built; tested on one machine (WSL2). Writer thread and timestamps are not built.

## What was built
- include/meridian/event_log.hpp: an 88-byte record per applied request: sequence, producer, tag, two timestamps (0 until M4), the 48-byte request, CRC over the first 84 bytes. A file header holds a magic, the record size and the byte order. The 48 request bytes are the same bytes as a Raft entry (doc 027); a test checks this against raft::encode_cmd for 3,000 random requests.
- EventLog::parse / load return the longest valid prefix and a status: Ok, IoError, BadHeader, Truncated, BadCrc, SeqGap, BadCmd (a request that decodes to an unknown enum value, even with a correct CRC).
- replay(log, oms, out) applies a log to a fresh Oms and returns every reply and a checksum over every fill, stop-triggered fills included.
- The Sequencer now keeps the same fills checksum while it runs (fills_digest()), so a replay can be compared to the original run.

## What the tests check (tests/test_event_log.cpp, 21 of 21 tests pass, event_log run 3 times)
- Codec: 3,000 random requests round-trip; the bytes equal the Raft entry; four bad enum values are refused.
- A 50-record log: round trip through bytes and through a file; a missing file gives IoError; cut at every byte offset gives BadHeader, Ok with the right prefix, or Truncated; one flipped byte in any of the 88 bytes of the first, middle and last record always gives BadCrc with the earlier records usable; a bad magic or record size gives BadHeader; a missing, repeated or swapped record gives SeqGap; an undecodable request with a correct CRC gives BadCmd.
- Replay: three runs of a real 4-producer sequencer (4,000 requests per producer, submits, cancels and modifies). The log is saved, loaded and replayed into a fresh Oms. Every reply, the OMS digest and the fills checksum equal the original run; a second replay equals the first; a log with one record removed does not.

## Not shown
- The tests were not run against deliberately broken versions of the log code on the author's machine (doubt 22).
- The log writer thread (design D5: own thread) and the timestamps are not built; the log is produced from the sequencer's applied() list after the run.
- Host byte order: a log is readable only on the same kind of machine. The header records the order but there is no converter.
- No timing: nothing here measures the cost of writing the log.

# 030: Binary TCP gateway v2 (sequencer milestone M3, first part)

## What was built
- include/meridian/protocol2.hpp: protocol version 2, 12-byte header, Submit 39 B, Cancel 24 B, Modify 32 B, Reply 43 B, Heartbeat 12 B. Prices are integer ticks. The client's message number comes back as client_seq.
- include/meridian/tcp_gateway.hpp: loopback server, one thread per connection, one producer slot of the Sequencer per connection, nonblocking IO, backpressure (outstanding + 1 < N), slots drained or retired on close, surplus connections closed at once, a malformed stream closes only its own connection.
- tests/test_tcp_gateway.cpp: parser tests and a test over real loopback connections.

## What the test checks
- Parser: every prefix of every message gives NeedMore, back-to-back messages split correctly, bad length, bad type, bad version and bad field are told apart, 200,000 random byte streams never read past their length.
- Over the wire (3 runs inside one test run): 3 clients pipeline 3,000 requests each; every request gets exactly one reply, in order, numbered as sent; a junk stream is closed and the gateway keeps serving; a fifth connection is closed at once; the event log has exactly as many entries as requests; replaying the log gives the same replies as the clients read, the same OMS digest and the same fills digest.

## Measured (author machine, WSL2)
- test_tcp_gateway: 5 runs, all print tcp_gateway ok. ctest: 22 of 22 pass (the full run took about 98 s).
- Eight deliberately broken variants of the parser and gateway were tried in the sandbox, not on the author machine; this is recorded in doubts.md.

## A warning that was not a bug
GCC -Warray-bounds reported a read past a 12-byte buffer in parse_one when it was inlined into the test's heartbeat check. The Submit branch cannot be reached with 12 bytes because the earlier length check returns NeedMore. The condition len >= sizeof(Submit) was added so the compiler can see it; the build then has 0 warnings and the test passes in 5 runs.

## Not done yet
- A standalone server program and the HTTP gateway routed through the sequencer, with a test that compares the logs from both paths (rest of M3).
- The cost of the yield loop while replies are owed is not measured. Latency is not measured at all (that is M4).
- Loopback on WSL2 only.

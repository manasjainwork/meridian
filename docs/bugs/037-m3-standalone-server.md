# 037: M3, standalone sequencer server and smoke test

## What was built
`src/seq_gateway.cpp` (CMake target `seq_gateway`): the sequencer on one thread and the binary TCP gateway (protocol v2) on loopback, up to 8 connections. It runs until SIGINT or SIGTERM, then stops the gateway, drains the sequencer, prints the gateway counters, replays its own event log to check it reproduces the live checksums, and saves the log if a file was given. `tools/ops/seq_smoke.py` is a Python client written from the protocol layout, not from the C++ headers.

## What was observed (author's machine, WSL2, one run)
The server was started on port 9111. The smoke client sent a buy of 10 at 500 (rested 10) and a sell of 4 at 500 (filled 4); the reply sequence numbers were consecutive. It sent a heartbeat and got it echoed, then sent a 12-byte message with a bad type, and the server closed the connection. After SIGINT the server reported 1 connection, 2 requests, 2 replies, 1 heartbeat, 1 bad message, and that the replay of its log reproduces the live checksums. The log of 2 records was saved.

## Not shown yet
- One run only, one connection, two orders. The multi-connection behaviour is covered by `test_tcp_gateway`, not by this smoke test.
- The smoke client is not part of ctest, so nothing runs it automatically.
- No load test through the server process itself; the latency numbers in bug 033 came from the in-process bench.
- Loopback only; no authentication or TLS.

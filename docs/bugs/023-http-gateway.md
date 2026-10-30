# 023: HTTP gateway in front of the order management layer

## What was built
`src/gateway.cpp`, a loopback-only (127.0.0.1) HTTP server. Prices are integer ticks and quantities integer lots, so nothing is rounded; `GET /config` says how many decimals a tick and a lot have. Endpoints: `POST /orders`, `POST /cancel`, `POST /modify`, `GET /book`, `GET /trades`, `GET /account`, `GET /config`, `GET /health`, and `GET /debug/check` (runs the full consistency check of doc 022). One thread per connection, one mutex around the OMS; the engine itself stays single-threaded. Limits: 16 KB of headers, 8 KB of body, 5 s to send a request, 64 connections.

## Test (`tools/gateway_test.py`), results from my machine
The script starts the binary and talks to it over real HTTP. It checks: a crossing trade with exact position and cash; the trade tape and the book; a retry with the same `cl_ord_id` is answered as a duplicate and trades nothing; the same key with another quantity is refused; 8 threads sending 25 retries each of one request at once give exactly 1 execution and 199 duplicates; cancel and modify by another account are refused; the owner can modify and cancel; cancelling twice is refused; the order size limit and the position limit (which counts open orders); malformed input (bad JSON, missing fields, negative number, bad side, a price above 32 bits, a 20000 byte body, wrong method, unknown path) gets a 4xx and the server stays alive; `/debug/check` passes at the end.
- 3 runs: ALL PASS each time.
- Mutation test over HTTP: the gateway can be started with one deliberate OMS bug (`--bug N`, tests only). The same script fails for every one: no idempotency (7 checks failed), any account can cancel (4), open quantity not reduced on cancel (1), taker not credited (4).

## Limits (not established)
- No authentication: the account number in the request is trusted. Fine on loopback, wrong for anything else.
- State lives in memory only: a restart forgets everything. Not behind Raft yet.
- Hand-written HTTP and JSON: no keep-alive, no JSON escapes, flat objects only. The tests above are the evidence it holds; it has not been fuzzed with a real HTTP fuzzer.
- Sanitizer builds (address, undefined, thread) have not been run on my machine yet.
- No latency numbers: this is a correctness step, not a measurement.
- `GET /book` scans at most 20000 ticks from the best price to find levels.
- CORS is open (`*`) so a page on another local port can call it; acceptable on loopback only.

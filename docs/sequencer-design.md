# Sequencer and gateway design

Date: 2026-11-09. Status: proposal. Only M1 (the sequencer core, `include/meridian/sequencer.hpp`) exists, with tests.
No number in this file is a Meridian result.

## Goal and scope
The sequencer is the one thread that orders every request and applies it to the engine, so that a run can be replayed exactly and each order's
latency can be measured. The research plan depends on it: the agents' orders must reach one engine in one recorded order, and the delay each
order saw must be written down.

In scope for version 1:
- One sequencer thread that owns the OMS and its single order book.
- A binary TCP gateway for agents, feeding the sequencer through lock-free rings.
- An event log (version 2) that holds every request, so the run can be replayed bit for bit.
- Four timestamps per order and an open-loop load generator that records them.
- A virtual-clock mode, so a simulated run does not depend on machine speed.

Out of scope for version 1:
- Raft integration. The sequencer's order becomes the proposal order later; the log record is chosen now so this needs no new format.
- More than one instrument.
- The Python Gym wrapper, the Redpanda publisher and the RL agents (each gets its own note).
- Any latency claim from WSL2. Latency numbers for claims need bare-metal Linux.

## What exists today
| Part | Where | State today | Gap for the sequencer |
|---|---|---|---|
| Order book and OMS | `order_book.hpp`, `oms.hpp` | Single-threaded by design; `submit`, `cancel`, `modify` take a request and return a result | Needs one owner thread |
| HTTP gateway | `src/gateway.cpp` | One thread per connection, one mutex around the OMS, JSON bodies, port 8081, loopback | A mutex is the opposite of a sequencer; HTTP is too heavy for agents |
| Binary protocol | `protocol.hpp` | 12-byte big-endian header with sequence number, `NewOrder` (38 B), `OrderAck` (29 B), stream parser (`NeedMore`, `BadLength`, `BadType`) | No cancel, modify, tif, account or stop fields |
| SPSC ring | `spsc_queue.hpp` | Tested, ThreadSanitizer clean; doc 009: about 4-5x faster than a mutex queue at p50 on WSL2 | Single producer only |
| Pipeline baseline | doc 010 | Ingress, ring, engine, ring, egress on pinned threads; open-loop latency; results identical to the single-thread run in 15 of 15 runs | WSL2, so not a benchmark |
| Journal | `journal.hpp`, `journal_writer.hpp` | 32-byte records with sequence and CRC; only `Add` and `Cancel`; CRC computed off the engine thread | Cannot hold modify, stops, accounts or time-in-force |
| Replicated request bytes | `raft_oms.hpp`, doc 027 | A 48-byte request record, encoded and decoded, carried in Raft log entries | Host byte order: a log format, not a wire format |
| Timing | `timing.hpp` | `rdtsc_begin`, `rdtsc_end` with fences, TSC calibration | Not yet used per order |
| Replay | `replay.hpp`, doc 011 | Replays the journal into a bare `OrderBook`; digest over results and fills | Drives the book, not the OMS; the sequencer needs an OMS-level replay (as in `tests/test_sequencer.cpp`) |

Two existing results shape the design. Doc 009 found SPSC throughput bimodal (about 17-30 M items/s in some runs, 175-325 M in others) with no
established cause. Doc 010 found millisecond stalls in single runs, cause untested. Neither blocks the design, but no sequencer latency figure
measured on this laptop can be a claim.

## Architecture

    agents (Python)
       | one TCP connection per agent
       v
    gateway threads (one per connection, pinned; stamp T1 on arrival)
       | one SPSC ring per connection (a full ring gives a "busy" reject, D7)
       v
    SEQUENCER THREAD  (owns OMS and book, assigns the sequence number; T2 at pop, T3 after apply)
       |-- reply (result, ack; T4 when written) --> back to the gateway thread
       '-- event log writer (own thread; request + header, D5)
                '-- replay checker (rebuilds book and fills from the log)

T1 arrival, T2 ring pop, T3 apply done, T4 ack written. The publisher reads the log later.

## Design decisions
Each row is a proposal with the alternative it rejects. "Check" is how the decision is tested before it is trusted.

| # | Decision | Rejected alternative | Reason | Check |
|---|---|---|---|---|
| D1 | One sequencer thread owns the OMS and assigns the sequence number | Mutex around the OMS, as the HTTP gateway does now | A mutex lets the OS decide the order and hides it; a single owner makes the order a recorded fact | Result checksum identical to the single-thread run, as in doc 010 |
| D2 | One SPSC ring per gateway connection, polled by the sequencer | A new multi-producer queue | Reuses the tested ring; a multi-producer queue needs its own proofs and mutation tests | ThreadSanitizer clean; ring-full test |
| D3 | Real-time mode polls rings starting from a rotating index | Always poll ring 0 first | A fixed order would favor one agent and corrupt the speed-advantage experiment | Share of ties each agent wins with identical load must be about even |
| D4 | Virtual-clock mode orders by event timestamp, ties broken by agent id | Wall-clock arrival order | A simulated run must not depend on machine speed or thread scheduling | Same input, same log, byte for byte, over repeated runs |
| D5 | Log record = the 48-byte request from doc 027 plus a header (sequence, ingress time, apply time, CRC) | Extend the 32-byte `Add`/`Cancel` journal record | One request format serves the journal and the Raft log; the old record cannot hold modify, stops or accounts | Round-trip test; truncation, bad-CRC and sequence-gap tests as in doc 011 |
| D6 | The log holds inputs only; outputs (acks, fills) come from replay | Log every output as well | The engine is deterministic, so outputs are reproducible; a smaller log is easier to verify | Replay reproduces the book digest and every fill |
| D7 | A full ring returns an explicit "busy" reject and a counter increments | Block the producer, or drop silently | A silent drop would change who traded; blocking makes the producer's latency lie | Test fills a ring and checks every request has exactly one outcome |
| D8 | Binary protocol v2 with cancel, modify, tif, account, stop price and an explicit reject code | Keep the v1 38-byte `NewOrder` | The OMS cannot be reached through v1 | Parser tests as for v1: split reads, bad length, bad type, fuzzed bytes |
| D9 | Thread per connection, pinned, for version 1 | `epoll` single I/O thread | The agent count is small (tens); `epoll` is a later change if the count grows | Defer; revisit if connections exceed available threads |
| D10 | The HTTP gateway stays for the UI but routes through the same sequencer | Keep two separate paths into the OMS | Two paths would mean two orders of events | One test drives both and compares logs |

**Timestamps.** Four points per order, each a TSC reading converted with the calibrated rate: arrival at the gateway, pop from the ring, end of OMS
apply, ack written. The first two give queueing delay, the middle two give engine time. TSC reliability across cores under WSL2 is not
established, so this is checked first.

**Latency injection.** In virtual-clock mode the simulation adds a delay to each message before the sequencer sees it. The delay comes either from
a fixed or random model, or from a distribution measured on the real engine. Comparing those two is hypothesis H4 of the literature note.

## Measurement plan
- **Open loop.** Latency is measured from the intended send time, so a stalled sequencer shows up as queueing delay (coordinated omission avoided).
- **Histogram.** Each of the three intervals (queueing, engine, ack) goes into an HdrHistogram; report p50, p99, p99.9 and max, never a mean alone.
- **Three runs minimum** at every rate before any statement, shown side by side; one-run outliers stay in the table, as in doc 010.
- **Correctness first.** Every load run also checks that the result checksum equals the single-thread run.
- **Calibration.** At start-up check that the TSC rate is stable and that readings from two pinned cores agree within a stated bound; if not, per-core timestamps are not compared.
- **Order-bias test (D3).** Several equal producers send the same load; count which one wins each tie. A large skew is reported and fixed before any speed-advantage experiment.
- **Labels.** Any number from WSL2 is an observation on that machine, not a benchmark. A latency claim waits for bare-metal Linux, with machine, kernel, pinning and governor recorded.
- **Sustained rate.** Doc 010 sent only 400k orders per rate (a burst). The sequencer tests need longer runs and a larger order pool.

## Build order and gates
Six milestones; each gate must pass before the next starts.

| Milestone | Gate | State |
|---|---|---|
| M1 Sequencer core | Checksum identical to the single-thread run; ThreadSanitizer clean; ring-full test: every request has exactly one outcome | Done on the author's machine (WSL2): tests pass in 3 runs; 6 of 7 mutants are caught (the 7th, ApplyTwice, is absorbed by OMS idempotency); ThreadSanitizer reported nothing in 3 runs at 5,000 ops per producer |
| M2 Event log v2 and replay | Round-trip, truncation, bad-CRC and sequence-gap tests; replay reproduces the book digest and every fill | Not started |
| M3 Binary TCP gateway v2 | Parser tests (split reads, bad length, bad type, fuzzed bytes); one test drives the HTTP and TCP paths and compares logs | Not started |
| M4 Timing | TSC calibration check passes; histograms from at least three runs per rate, checksum still identical | Not started |
| M5 Virtual-clock mode | Same input gives the same log byte for byte over repeated runs; the tie-break mutation is caught | Not started |
| M6 Downstream publisher | The published stream equals the replay output for the same log | Not started |

**Mutation tests.** Meridian's method is to inject a known bug and prove a test catches it (12 flags in the Raft node, 7 in the OMS). The sequencer
gets the same: dropping a request when a ring is full, applying a request twice, skipping a sequence number, taking the ingress timestamp after
the apply, breaking ties by arrival instead of by agent id in virtual-clock mode. The count is not fixed; each bug found while building adds one.

## Risks and unknowns
| Risk or unknown | Why it matters | First step |
|---|---|---|
| Poll order biases ties in real-time mode | It could manufacture a speed advantage that the experiment then "finds" | Order-bias test before any H2 run |
| TSC across cores under WSL2 is untested | Per-order timestamps from different cores may not be comparable | Calibration check; stamp and read on the same core where possible |
| SPSC throughput was bimodal with no known cause (doc 009) | The sequencer is built on the same ring | Re-test the ring on bare-metal Linux before relying on a throughput number |
| Millisecond stalls on WSL2 (doc 010) | They would swamp tail latency | Treat WSL2 results as observations; plan the bare-metal run |
| Old 32-byte journal and new log must not both be authoritative | Two formats invite a replay that disagrees with the run | Keep the old reader for old files only; new runs write version 2 only |
| Single instrument | `instrument_id` exists in the protocol, but the OMS owns one book | State it in the log header; a second instrument is a later design |
| Raft integration deferred | Log record and sequence number must not need to change when it arrives | Keep the record equal to the doc 027 request bytes plus a header |

## Open question
Should the first agents connect over TCP on loopback, or should the Python side call the engine in-process through a binding? This design assumes
TCP, because it also measures the network path; an in-process binding would be faster for training but would skip the path that H4 studies.

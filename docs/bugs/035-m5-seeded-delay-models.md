# 035: M5 slice 2, seeded delay models

## What was built
`include/meridian/delay_model.hpp`: `DelayModel` (fixed, uniform range, or drawn from a list of measured delays) and `agent_rng(seed, agent)`, one generator per agent derived from the seed and the agent id. Draws use `std::mt19937_64` (fully specified by the C++ standard) and plain modulo, not `std::uniform_int_distribution`, whose output differs between standard libraries. Modulo has a tiny bias for ranges that do not divide 2^64; it is not corrected.

## What the test shows (`tests/test_delay_model.cpp`, ctest `delay_model`)
- Draws stay in range, the uniform model hits both ends of its range, the empirical model returns only its samples and all of them, an empty sample list gives 0.
- `agent_rng` repeats for the same (seed, agent) and differs for another agent or seed.
- 5 agents x 800 messages: the same seed gave identical event log bytes on 3 repeated runs and for two different push orders (agent by agent, and round-robin). Another seed gave different bytes. The same held for an empirical model.
- With one generator shared by all agents, the two push orders gave different logs. That is the mistake per-agent generators avoid, and the test detects it.
- Run 3 times on the author's machine, all passed.

## Not shown yet
- Delays measured from a real run were not fed in; only hand-written sample lists were tested (H4 still open).
- Same bytes on another machine or compiler were not checked.
- The shape of real delays (heavy tail, correlation between messages) is not modelled; draws are independent.

# Measurement methodology

1. Every number in the README comes from a binary in bench/ and a log in docs/.
2. Report p50, p99, p99.9 and max, never the mean alone.
3. Warm up before recording; record sample count and TSC frequency.
4. State the environment: WSL2 vs bare metal, compiler, flags, date.
5. Subtract known timer overhead (~32 cycles) and say so.
6. Never compare numbers from different machines or environments.
7. Every bug gets a failing test first, then the fix, then docs/bugs/NNN.md.
8. A claim I did not reproduce myself is a hypothesis, not a fact.

# 008: Where the ~450 ns p99.9 comes from

## Observation
Mixed-workload book latency: p99 ~100-130 ns, p99.9 ~440-460 ns, identical for rest/match/cancel
and for linear-scan and bitmap builds (see 007).

## Experiments (WSL2, TSC 2.688 GHz)
- Empty timed region: p99.9 0.7-5 ns. Timer/VM floor is clean, not the cause.
- L1-resident load: p99.9 2-3 ns.
- Random load over >=8 MB: p50 85-113 ns, p99 ~440-480 ns, nearly independent of size up to 512 MB.
- Same with madvise(MADV_HUGEPAGE) vs NOHUGEPAGE: no consistent difference at any size.

## Conclusion (what is supported)
The book's p99 matches one cache-missing load (~90 ns) and its p99.9 matches the
p99 of a cache-missing load (~450 ns). The tail is memory-access latency, not timer noise,
not the best-price scan, and not (as far as guest hugepages can show) TLB reach.

## Not established
Why a cache-missing load reaches ~450 ns. Candidate: host-side nested paging under WSL2
(guest THP does not control host mapping). Needs bare-metal Linux to test.

## Consequence
Remaining lever inside the engine: fewer cache-missing loads per operation (layout, prefetch).
Tuning (isolcpus, pinned hugepages) must be re-measured off WSL2 before claiming anything.

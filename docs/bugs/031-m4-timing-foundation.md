# 031: Timing foundation for M4 (TSC check and latency histogram)

## What was built
- bench/tsc_check.cpp: prints the CPU flags and clock source, calibrates the TSC rate five times, and for core 0 against cores 1, 2 and 3 estimates the offset between their TSCs with a ping-pong (best of 20,000 round trips; uncertainty = half the best round trip), twice, one second apart.
- include/meridian/hdr_histogram.hpp: log-linear histogram (k = 7, fixed 59 x 128 counters, no allocation after construction). Values below 256 are exact; larger values are within 2^-7 of the true value; a percentile is the upper edge of its bucket, never below the true value.
- tests/test_hdr_histogram.cpp (ctest name hdr_histogram).

## Results (author machine, WSL2, 3 runs of tsc_check)
- Flags: constant_tsc, nonstop_tsc, tsc_reliable, tsc_known_freq, rdtscp, hypervisor all yes; clocksource tsc.
- Calibration: 2.688011 GHz in all 15 windows; spread 0.1 to 0.2 ppm.
- Cross-core, cores 0 against 1, 2, 3: offsets between -25 and +6 ns, uncertainty +/- 60 to 74 ns, change over one second between -23 and +21 ns; all 9 pair measurements consistent with zero offset.
- test_hdr_histogram: ok in 3 runs, 0 compiler warnings.

## What this lets us say, and what not
- On this machine, timestamps taken on cores 0 to 3 can be compared to within about 70 ns. A queueing delay shorter than that, measured across two cores, is not resolved.
- The calibration is against steady_clock; under WSL2 that clock may itself be derived from the TSC, so the tiny spread does not prove the TSC is stable against real time.
- Only cores 0 to 3 of 12 were compared, and WSL2 does not say which logical cores share a physical core.
- Broken-variant check of the histogram test was done only in the assistant's sandbox (4 of 4 behaviour-changing variants caught; one more variant changed nothing and is equivalent). Not reproduced on the author machine.

## Not done yet
- T2 and T3 stamps in the sequencer, T1 and T4 in the gateway, the open-loop client, histograms of queueing, engine and ack time from at least three runs per rate, and the check that the result checksum still equals the single-thread run.

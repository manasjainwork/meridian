# 001: Timer overhead and WSL2 jitter

## Setup
WSL2 (Ubuntu), g++ 15.2, -O2 Release, TSC 2.688 GHz, 1,000,000 samples of
back-to-back rdtsc_begin()/rdtsc_end() with lfence.

## Result
| pct   | cycles | ns     |
|-------|--------|--------|
| min   | 28     | 10.4   |
| p50   | 32     | 11.9   |
| p99   | 34     | 12.6   |
| p99.9 | 36     | 13.4   |
| max   | 20286  | 7546.9 |

## Observations
- The timer itself costs ~32 cycles (~12 ns). Any measurement below that is noise.
- The body of the distribution is tight (p50 to p99.9 is 4 cycles).
- The max is ~630x the median. A handful of samples got preempted, most likely
  by the Windows host or Hyper-V scheduler, since WSL2 is a VM.

## What I will do about it
- Subtract the ~32 cycle overhead from later benchmarks.
- Report p99.9 and max, never only the mean.
- Month 6: re-run on an isolated core (isolcpus, SCHED_FIFO) and compare.

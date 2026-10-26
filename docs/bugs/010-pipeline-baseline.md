# 010: Three-thread pipeline baseline

## Setup
Ingress -> SpscQueue -> engine (OrderBook) -> SpscQueue -> egress. Three pinned threads
(cpu 0, 2, 4), 400k orders (70% limit, 30% cancel), open loop: latency is measured from the
intended send time to the ack reaching egress, so engine stalls show up as queueing delay.
First 10k orders discarded. Environment: WSL2, i5-11400H, TSC 2.688 GHz, 3 runs.

## Correctness
15/15 (rates x runs): fills (198016) and result checksum identical to the single-thread run.

## Results (ns, 3 runs)
- p50: 140-400 (typically ~250) for 0.5-4 M orders/s
- p99: 4.7-20 us for 0.5-2 M orders/s; 20-49 us at 4 M orders/s
- p99.9: 17-130 us; 110-527 us at 4 M orders/s
- Outliers: p99.9 up to 15.6 ms at 100k/s and p99 7.6 ms at 2 M/s in one run each

## Not established
- Cause of the ms-scale stalls (hypothesis: a thread descheduled by Windows/WSL2). Not tested.
- Why p50 at 100k/s (~380-400) is higher than at 500k/s (~250) (hypothesis: cold cache lines).
- Saturation point: not reached at 4 M orders/s.

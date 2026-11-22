# 036: M5 slice 3, virtual-clock sequencer against the real-time sequencer

## What was tested (`tests/test_virtual_vs_realtime.cpp`, ctest `virtual_vs_realtime`)
Three producer threads sent 10000 requests each (submits and cancels) to the real-time sequencer. The order it applied them in was then fed to the virtual sequencer, one virtual microsecond apart with no delay. In 3 runs on the author's machine the replies, the OMS digest and the fills digest were identical to the real run. A feed with two neighbouring entries from different producers swapped gave a different state each time, so the comparison can fail.

## What this shows and does not show
- Shows: the virtual sequencer applies requests exactly like the real-time one when given the same order.
- Does not show: that real-time ordering is reproducible. It is not; which producer wins a race changes from run to run. That is why the virtual clock exists.
- Event log bytes were not compared, because timestamps differ (wall clock against virtual time).
- Delays measured from a real run were not fed in (H4 still open).

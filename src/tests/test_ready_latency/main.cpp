#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include "ready_latency.h"

using namespace brook;
int main() {
    ReadyLatencyTracker<8> tracker;
    tracker.Enqueue(1, 10, false, 100); tracker.Dispatch(1, 10, 200);
    assert(!tracker.Snapshot().user.samples && !tracker.Enabled());
    tracker.Enable(true);
    tracker.SeedPending(7, 70, false); tracker.Enqueue(7, 70, false, 100);
    tracker.Dispatch(7, 70, 10000);
    assert(tracker.Snapshot().preexistingDispatches == 1 && !tracker.Snapshot().user.samples);
    tracker.Enqueue(1, 10, false, 100);
    tracker.Enqueue(1, 10, false, 900); // A retry cannot hide queue latency.
    tracker.Dispatch(1, 10, 1100);
    auto snapshot = tracker.Snapshot();
    assert(snapshot.user.samples == 1 && snapshot.user.maximumNs == 1000);
    assert(snapshot.enqueues == 1 && !snapshot.pending && snapshot.user.bins[0] == 1);
    tracker.Enqueue(2, 20, true, 1000); tracker.Dispatch(2, 20, 6001);
    assert(tracker.Snapshot().kernel.bins[2] == 1);
    tracker.Enqueue(3, 30, false, 1000); tracker.Remove(3, 29);
    assert(tracker.Snapshot().pending == 1);
    tracker.Remove(3, 30); tracker.Dispatch(3, 30, 5000);
    snapshot = tracker.Snapshot();
    assert(!snapshot.pending && snapshot.cancelled == 1 && snapshot.untrackedDispatches == 1);
    // PID reuse retires the old episode; its timestamp cannot cross identity.
    tracker.Enqueue(4, 40, false, 1000); tracker.Enqueue(4, 41, false, 5000);
    tracker.Dispatch(4, 40, 6000); assert(tracker.Snapshot().pending == 1);
    tracker.Dispatch(4, 41, 7000);
    snapshot = tracker.Snapshot();
    assert(snapshot.user.sumNs == 3000 && snapshot.user.maximumIncarnation == 41);
    assert(snapshot.cancelled == 2 && !snapshot.pending);
    tracker.Enqueue(5, 50, false, 0); tracker.Dispatch(5, 50, 1000);
    tracker.Enqueue(5, 50, false, 2000); tracker.Dispatch(5, 50, 0);
    tracker.Enqueue(5, 50, false, 2000); tracker.Dispatch(5, 50, 1999);
    snapshot = tracker.Snapshot();
    assert(snapshot.unavailableClock == 2 && snapshot.backwardsClock == 1);
    assert(snapshot.user.samples == 2);
    tracker.Enqueue(6, 60, false, 1000);
    const auto beforeOff = tracker.Snapshot(); tracker.Enable(false);
    tracker.Dispatch(6, 60, 9000); tracker.Remove(6, 60);
    assert(tracker.Snapshot().pending == beforeOff.pending);
    tracker.Enable(true);
    assert(!tracker.Snapshot().pending && !tracker.Snapshot().user.samples);
    tracker.Dispatch(6, 60, 10000); assert(tracker.Snapshot().untrackedDispatches == 1);
    tracker.Enqueue(0, 1, false, 1); tracker.Enqueue(8, 1, false, 1);
    assert(!tracker.Snapshot().pending);
    // Exercise every inclusive histogram boundary and the overflow bin.
    tracker.Reset();
    for (unsigned i = 0; i + 1 < ReadyLatencyBins; ++i) {
        tracker.Enqueue(1, 10, false, 1); tracker.Dispatch(1, 10, ReadyLatencyBoundsNs[i] + 1);
    }
    tracker.Enqueue(1, 10, false, 1); tracker.Dispatch(1, 10, 2000000001);
    snapshot = tracker.Snapshot();
    for (auto count : snapshot.user.bins) assert(count == 1);
    assert(snapshot.user.samples == ReadyLatencyBins && !snapshot.pending);
    tracker.Reset();
    tracker.Enqueue(1, 10, false, 1); tracker.Dispatch(1, 10, ~uint64_t{0});
    tracker.Enqueue(1, 10, false, 1); tracker.Dispatch(1, 10, 100);
    assert(tracker.Snapshot().user.sumSaturated && tracker.Snapshot().user.sumNs == ~uint64_t{0});
    // Watch selection binds an incarnation, preserves aggregate diagnostics,
    // and never invents a queue start for a task already queued at selection.
    tracker.Reset();tracker.Watch(2,20);
    tracker.Enqueue(2,20,true,100);tracker.Deadline(2,20,10,240);
    tracker.Dispatch(2,20,1000100);
    auto watched=tracker.Snapshot().watch;
    assert(watched.samples==1 && watched.timedSamples==1 && watched.maxLateMs==230);
    assert(watched.maxQueueNs==1000000 && watched.events[0].readyNs==100);
    assert(watched.events[0].dispatchNs==1000100 && watched.events[0].hasDeadline);
    // A runnable retry preserves original queue time and no-deadline wakeups
    // do not acquire a stale deadline from the previous sleep.
    tracker.Enqueue(2,20,true,2000000);tracker.Enqueue(2,20,true,2000100);
    tracker.Dispatch(2,20,32000000);
    watched=tracker.Snapshot().watch;
    assert(watched.samples==2 && watched.timedSamples==1 && watched.events[1].queueNs==30000000);
    assert(!watched.events[1].hasDeadline);
    tracker.Enqueue(2,20,true,33000000);tracker.Remove(2,19);
    assert(tracker.Snapshot().watch.pending);
    tracker.Remove(2,20);assert(!tracker.Snapshot().watch.pending);
    tracker.Enqueue(2,20,true,34000000);tracker.Enqueue(2,21,true,35000000);
    tracker.Deadline(2,21,1,1000);tracker.Dispatch(2,21,90000000);
    assert(tracker.Snapshot().watch.samples==2 && !tracker.Snapshot().watch.pending);
    tracker.Watch(2,21);tracker.Enqueue(2,21,true,100000000);
    tracker.Deadline(2,21,1000,900);tracker.Dispatch(2,21,100000001);
    watched=tracker.Snapshot().watch;assert(watched.samples==1 && !watched.maxLateMs && watched.timedSamples==1);
    tracker.Reset();assert(tracker.Snapshot().watch.pid==2 && !tracker.Snapshot().watch.samples);
    tracker.Enqueue(2,21,true,0);tracker.Dispatch(2,21,1);
    assert(tracker.Snapshot().watch.invalid==1);
    tracker.Enqueue(2,21,true,10);tracker.Dispatch(2,21,9);
    assert(tracker.Snapshot().watch.invalid==2);
    tracker.Reset();tracker.SeedPending(2,21,true);tracker.Dispatch(2,21,100000000);
    assert(!tracker.Snapshot().watch.samples && !tracker.Snapshot().watch.pending);
    tracker.Enqueue(2,21,true,1);tracker.Watch(2,21);tracker.Dispatch(2,21,90000000);
    assert(!tracker.Snapshot().watch.samples); // selected while already queued
    for(unsigned i=0;i<40;++i){
        tracker.Enqueue(2,21,true,1);tracker.Deadline(2,21,1,21+i);tracker.Dispatch(2,21,2);
    }
    watched=tracker.Snapshot().watch;
    assert(watched.slow==40 && watched.retained==16 && watched.excluded==24);
    assert(watched.events[0].lateMs==59 && watched.events[15].lateMs==44);
    tracker.Enable(false);tracker.Dispatch(2,21,1000);tracker.Enable(true);
    assert(tracker.Snapshot().watch.pid==2 && !tracker.Snapshot().watch.samples);
    tracker.Watch(0,0);assert(!tracker.Watching(2,21));
    std::puts("READY_LATENCY_PASS identities, queue retries, cancellation, clock bounds, histogram, opt-in/reset");
}

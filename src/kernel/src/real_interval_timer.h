#pragma once
#include <stdint.h>
#include "timeout_value.h"

namespace brook {
// State for one process-wide ITIMER_REAL, serialized by the signal-group lock.
// The caller supplies a monotonic nanosecond clock and owns signal delivery.
class RealIntervalTimer {
    uint64_t deadline_=0, interval_=0;
    bool armed_=false, pending_=false;
public:
    struct Value { uint64_t remaining,interval; };
    Value Read(uint64_t now) const {
        // Linux reports one microsecond when an armed timer is overdue but
        // its expiry callback has not yet run.
        return {armed_?(deadline_>now?deadline_-now:1000):0,interval_};
    }
    Value Set(uint64_t now,uint64_t initial,uint64_t interval) {
        const Value old=Read(now);
        interval_=initial?interval:0;armed_=initial!=0;
        deadline_=armed_?TimeoutAddSafe(now,initial):0;
        // Reprogramming must not remove an already pending SIGALRM.
        return old;
    }
    bool Expire(uint64_t now) {
        if(!armed_ || now<deadline_)return false;
        armed_=false;pending_=true;
        return true;
    }
    bool Pending() const {return pending_;}
    // An ignored notification is discarded, not delivered; unlike consuming
    // it, discarding does not restart an expired periodic REAL timer.
    void Discard() {pending_=false;}
    // Called for any process-directed SIGALRM dequeue, even when another
    // sender supplied the coalesced signal. Thread-directed signals differ.
    bool SharedAlarmDequeued(uint64_t now) {
        const bool wasPending=pending_;pending_=false;
        if(!armed_ && interval_) {
            const uint64_t elapsed=now>=deadline_?now-deadline_:0;
            deadline_=TimeoutAddSafe(now,interval_-(elapsed%interval_));
            armed_=true;
        }
        return wasPending;
    }
};
}

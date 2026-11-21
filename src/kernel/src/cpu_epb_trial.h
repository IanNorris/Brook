#pragma once

#include <stdint.h>

namespace brook {

// All calls are serialized. Tick must run on the CPU whose EPB it accesses.
class CpuEpbTrial {
public:
    static constexpr unsigned MaxCpus = 8;
    static constexpr uint64_t SetupMs = 3000;
    enum class Phase : uint8_t { Off, Snapshot, Apply, Hold, Restore };
    enum class Outcome : uint8_t { None, Complete, Cancelled, Ineligible, Timeout, Readback, Superseded };

    bool Start(uint64_t now, unsigned cpus, unsigned target, uint32_t durationMs)
    {
        if (phase!=Phase::Off || !cpus || cpus>MaxCpus || target>15 ||
            durationMs<1000 || durationMs>300000) return false;
        *this={};
        cpuCount=cpus;
        expected=(1u<<cpus)-1;
        value=target;
        duration=durationMs;
        phase=Phase::Snapshot;
        deadline=now+SetupMs;
        return true;
    }

    void Cancel(Outcome reason=Outcome::Cancelled)
    {
        if (phase==Phase::Off || phase==Phase::Restore) return;
        outcome=reason;
        deadline=0;
        phase=touched?Phase::Restore:Phase::Off;
    }

    template<class Read,class Write>
    void Tick(unsigned cpu,uint64_t now,bool eligible,Read read,Write write)
    {
        if (phase==Phase::Off || cpu>=cpuCount) return;
        if (!eligible) Cancel(Outcome::Ineligible);
        if ((phase==Phase::Snapshot || phase==Phase::Apply) && now>=deadline)
            Cancel(Outcome::Timeout);
        if (phase==Phase::Hold && now>=deadline)
            Cancel(Outcome::Complete);

        const unsigned bit=1u<<cpu;
        if (phase==Phase::Snapshot) {
            if (!(sampled&bit)) {
                observed[cpu]=original[cpu]=read();
                sampled|=bit;
            }
            // All logical CPUs must snapshot before any writes. This also
            // handles firmware that shares the bias among siblings.
            if (sampled==expected) {
                phase=Phase::Apply;
                deadline=now+SetupMs;
            }
        } else if (phase==Phase::Apply && !(touched&bit)) {
            const unsigned current=read();
            if (current!=original[cpu] && current!=value) {
                observed[cpu]=current;
                Cancel(Outcome::Superseded);
                return;
            }
            touched|=bit;
            observed[cpu]=current==value?current:write(value);
            if (observed[cpu]!=value) {
                Cancel(Outcome::Readback);
            } else if (touched==expected) {
                phase=Phase::Hold;
                deadline=now+duration;
            }
        } else if (phase==Phase::Hold) {
            observed[cpu]=read();
            if (observed[cpu]!=value) Cancel(Outcome::Superseded);
        } else if (phase==Phase::Restore && (touched&bit) && !(restored&bit)) {
            const unsigned current=read();
            observed[cpu]=current;
            // A newer bias from firmware must not be overwritten. A
            // sibling may also have restored a shared setting already.
            if (current==value && current!=original[cpu]) observed[cpu]=write(original[cpu]);
            if (observed[cpu]==original[cpu]) {
                restored|=bit;
            } else if (observed[cpu]!=value || ++restoreAttempts[cpu]==3) {
                restoreFailed|=bit;
                restored|=bit;
            }
            if ((restored&touched)==touched) phase=Phase::Off;
        }
    }

    Phase phase=Phase::Off;
    Outcome outcome=Outcome::None;
    uint64_t deadline=0;
    unsigned cpuCount=0,value=0,sampled=0,touched=0,restored=0,restoreFailed=0;
    uint8_t original[MaxCpus]{},observed[MaxCpus]{};

private:
    unsigned expected=0;
    uint32_t duration=0;
    uint8_t restoreAttempts[MaxCpus]{};
};

} // namespace brook

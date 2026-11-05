#include "../../kernel/src/panic_reset_timer.h"
#include <cassert>
#include <cstdint>
#include <vector>

struct Pit {
    uint8_t gate = 0xa2;
    unsigned intervals = 0, samples = 0, resetCalls = 0, controlCalls = 0;
    bool stuckHigh = false, stuckLow = false;
    std::vector<uint8_t> counters;
    uint8_t readGate() {
        if (stuckHigh) return uint8_t(gate | 0x20);
        if (stuckLow) return uint8_t(gate & ~0x20u);
        if (gate & 1) return ++samples >= 3 ? uint8_t(gate | 0x20) : uint8_t(gate & ~0x20u);
        return uint8_t(gate & ~0x20u);
    }
    void writeGate(uint8_t v) { if (!(gate & 1) && (v & 1)) { ++intervals; samples=0; } gate=v; }
    void writeControl(uint8_t v) { assert(v == 0xb0); ++controlCalls; }
    void writeCounter(uint8_t v) { counters.push_back(v); }
    void pause() {}
    void issueReset() { ++resetCalls; }
};
int main() {
    using brook::PanicResetAfterIntervals;
    using brook::PanicResetResult;
    Pit normal;
    assert(PanicResetAfterIntervals(normal, 3, 10) == PanicResetResult::ResetReturned);
    assert(normal.intervals == 3 && normal.controlCalls == 3 && normal.resetCalls == 1);
    assert(normal.gate == 0x82); // saved input had output bit 5 clear in this fake
    assert((normal.counters == std::vector<uint8_t>{0x0b,0xe9,0x0b,0xe9,0x0b,0xe9}));
    Pit high; high.stuckHigh=true;
    assert(PanicResetAfterIntervals(high, 1, 4) == PanicResetResult::TimerFailed);
    assert(high.resetCalls == 0 && high.gate == 0xa2);
    Pit low; low.stuckLow=true;
    assert(PanicResetAfterIntervals(low, 1, 4) == PanicResetResult::TimerFailed);
    assert(low.resetCalls == 0 && low.gate == 0x82);
    Pit invalid;
    assert(PanicResetAfterIntervals(invalid, 0, 3) == PanicResetResult::Invalid);
    assert(PanicResetAfterIntervals(invalid, 12001, 3) == PanicResetResult::Invalid);
    assert(PanicResetAfterIntervals(invalid, 1, 0) == PanicResetResult::Invalid);
    assert(invalid.resetCalls == 0 && invalid.controlCalls == 0);
}

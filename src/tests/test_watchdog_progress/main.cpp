#include "../../kernel/src/watchdog_progress.h"
#include <cassert>

int main() {
    brook::WatchdogProgress cpu;
    cpu.Reset(10);
    for (int i=0;i<10;++i) assert(!cpu.Observe(10,3)); // delayed AP: unarmed
    assert(!cpu.Observe(11,3));
    assert(!cpu.Observe(11,3));
    assert(!cpu.Observe(11,3));
    assert(cpu.Observe(11,3));
    for (int i=0;i<10;++i) assert(!cpu.Observe(11,3)); // only one report
    assert(!cpu.Observe(12,3)); // recovery rearms
    assert(!cpu.Observe(12,3));
    assert(!cpu.Observe(13,3)); // progress resets a partial run
    assert(!cpu.Observe(13,3));
    assert(!cpu.Observe(13,3));
    assert(cpu.Observe(13,3));
    cpu.Reset(0);
    assert(!cpu.Observe(1,0));
    assert(!cpu.Observe(1,0));
}

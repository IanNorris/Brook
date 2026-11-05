#include "../../kernel/src/ahci_panic_gate.h"
#include <atomic>
#include <cassert>
#include <thread>
using brook::AhciPanicGate;
int main() {
    AhciPanicGate gate;
    assert(gate.EnterNormal());
    assert(!gate.EnterPanic());
    assert(!gate.EnterNormal());
    gate.LeaveNormal();
    assert(gate.EnterPanic());
    assert(!gate.EnterNormal());
    assert(!gate.EnterPanic());
    // Race an ordinary holder with panic admission. Both must never win.
    for (unsigned i=0;i<1000;++i) {
        AhciPanicGate race;
        std::atomic<bool> start{false},normal{false},panic{false},release{false};
        std::thread io([&] {while(!start.load(std::memory_order_acquire)){};
            normal.store(race.EnterNormal(),std::memory_order_release);
            while(!release.load(std::memory_order_acquire)){};
            if(normal.load(std::memory_order_acquire))race.LeaveNormal();});
        std::thread crash([&] {while(!start.load(std::memory_order_acquire)){};
            panic.store(race.EnterPanic(),std::memory_order_release);});
        start.store(true,std::memory_order_release);
        crash.join();
        // Wait until the ordinary admission attempt has happened without
        // allowing it to release ownership first.
        while(!normal.load(std::memory_order_acquire) && !panic.load(std::memory_order_acquire)) {}
        assert(!(normal.load(std::memory_order_acquire) && panic.load(std::memory_order_acquire)));
        release.store(true,std::memory_order_release);io.join();
        if(panic.load(std::memory_order_acquire))assert(!race.EnterNormal());
    }
}

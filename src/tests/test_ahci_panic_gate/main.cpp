#include "../../kernel/src/ahci_panic_gate.h"
#include <atomic>
#include <cassert>
#include <thread>
using brook::AhciPanicGate;
int main() {
    using Request=AhciPanicGate::Request;
    struct Timer {
        AhciPanicGate& gate;uint32_t now,step,reads=0,releaseAt=0;
        uint32_t readTimer(){++reads;uint32_t result=now;now+=step;return result;}
        void pause(){if(releaseAt && reads==releaseAt)gate.LeaveNormal();}
    };
    {AhciPanicGate g;assert(g.EnterNormal() && g.RequestPanic()==Request::Pending);
     Timer t{g,0xffff00,1000000,0,4};assert(brook::AhciWaitForPanicOwner(g,t,20) && g.PanicOwned());}
    {AhciPanicGate g;assert(g.EnterNormal() && g.RequestPanic()==Request::Pending);
     Timer t{g,0xffff00,1000000};assert(!brook::AhciWaitForPanicOwner(g,t,20) && g.PanicPending() && t.reads<20);}
    {AhciPanicGate g;assert(g.EnterNormal() && g.RequestPanic()==Request::Pending);
     Timer t{g,42,0};assert(!brook::AhciWaitForPanicOwner(g,t,5) && g.PanicPending() && t.reads==6);}
    AhciPanicGate gate;
    assert(gate.EnterNormal());
    assert(!gate.EnterPanic());
    AhciPanicGate pending;
    assert(pending.EnterNormal());
    assert(pending.RequestPanic()==Request::Pending);
    assert(pending.PanicPending());
    assert(!pending.PanicOwned() && !pending.EnterNormal() && !pending.EnterPanic());
    assert(pending.RequestPanic()==Request::Unavailable);
    pending.LeaveNormal();
    assert(pending.PanicOwned() && !pending.PanicPending() && !pending.EnterNormal());
    AhciPanicGate immediate;
    assert(immediate.RequestPanic()==Request::Acquired && immediate.PanicOwned());
    assert(!immediate.EnterNormal());
    // An admitted normal operation hands ownership directly to panic; no
    // intervening ordinary caller can acquire the gate.
    for(unsigned i=0;i<1000;++i) {
        AhciPanicGate transfer;assert(transfer.EnterNormal());
        std::atomic<bool> start{false};AhciPanicGate::Request result=Request::Unavailable;
        std::thread requester([&]{while(!start.load(std::memory_order_acquire)){};result=transfer.RequestPanic();});
        start.store(true,std::memory_order_release);transfer.LeaveNormal();requester.join();
        assert(result==Request::Pending || result==Request::Acquired);
        assert(transfer.PanicOwned() && !transfer.EnterNormal());
    }
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

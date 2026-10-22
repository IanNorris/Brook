#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include "mount_namespace_state.h"
using brook::MountNamespaceState;

int main() {
    // A copied context/snapshot is storage, not an extra namespace member.
    MountNamespaceState state;
    assert(!state.Published() && !state.IsRetired());
    assert(!state.TryJoin());
    state.RetainStorage(); // unpublished copied context
    assert(!state.Published());
    assert(state.ActivatePrepared());
    assert(!state.ActivatePrepared());
    state.RetainStorage(); // operation snapshot surviving task exit
    assert(state.Published());
    assert(state.LeaveMember());
    assert(!state.Published() && state.IsRetired());
    assert(!state.TryJoin() && !state.ActivatePrepared());
    assert(!state.ReleaseStorage()); // task storage
    assert(!state.ReleaseStorage()); // copied context
    assert(!state.Published());       // operation snapshot cannot revive mounts
    assert(state.ReleaseStorage());  // final deferred destruction may run now

    MountNamespaceState failedClone;
    failedClone.RetainStorage();
    assert(failedClone.AbandonPrepared());
    assert(!failedClone.AbandonPrepared());
    assert(!failedClone.TryJoin() && !failedClone.ActivatePrepared());
    assert(!failedClone.ReleaseStorage());
    assert(failedClone.ReleaseStorage());

    // Existing tasks sharing one FsContext still hold separate member leases.
    MountNamespaceState shared;
    assert(shared.ActivatePrepared());
    for(int i=0;i<7;++i) { shared.RetainStorage();assert(shared.TryJoin()); }
    for(int i=0;i<7;++i) {
        assert(!shared.LeaveMember());assert(shared.Published());
        assert(!shared.ReleaseStorage());
    }
    assert(shared.LeaveMember());
    assert(!shared.Published());
    assert(shared.ReleaseStorage());

    // Concurrent joins, snapshots, and member departures. The root task keeps
    // publication alive until all workers have taken their initial lease.
    for(int run=0;run<40;++run) {
        MountNamespaceState ns;
        assert(ns.ActivatePrepared());
        std::atomic<unsigned> ready{0},last{0};std::atomic<bool> go{false};
        std::vector<std::thread> workers;
        for(int i=0;i<8;++i) {
            ns.RetainStorage();
            assert(ns.TryJoin());
            workers.emplace_back([&] {
                ready.fetch_add(1,std::memory_order_release);
                while(!go.load(std::memory_order_acquire))std::this_thread::yield();
                for(int j=0;j<1000;++j) {
                    ns.RetainStorage();assert(ns.Published());
                    assert(!ns.ReleaseStorage());
                    // A temporary additional member cannot be the last lease.
                    assert(ns.TryJoin());assert(!ns.LeaveMember());
                }
                if(ns.LeaveMember())last.fetch_add(1);
                // Main retains storage throughout so this cannot free ns.
                assert(!ns.ReleaseStorage());
            });
        }
        while(ready.load(std::memory_order_acquire)!=8)std::this_thread::yield();
        assert(!ns.LeaveMember());go.store(true,std::memory_order_release);
        for(auto& t:workers)t.join();
        assert(last.load()==1 && !ns.Published() && ns.IsRetired());
        assert(!ns.TryJoin());assert(ns.ReleaseStorage());
    }
    // Joining concurrently with the last departure either wins a live member
    // or fails. A join after retirement must never succeed, even with storage.
    for(int run=0;run<1000;++run) {
        MountNamespaceState ns;assert(ns.ActivatePrepared());ns.RetainStorage();
        std::atomic<bool> go{false};std::atomic<unsigned> last{0};
        std::thread t([&] {
            while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            if(ns.TryJoin() && ns.LeaveMember())last.fetch_add(1);
            assert(!ns.ReleaseStorage());
        });
        go.store(true,std::memory_order_release);
        if(ns.LeaveMember())last.fetch_add(1);
        t.join();assert(last.load()==1 && ns.IsRetired() && !ns.Published());
        assert(!ns.TryJoin());assert(ns.ReleaseStorage());
    }
    std::puts("PASS namespace member/publication/storage lifetimes and concurrent last departure; topology not integrated");
}

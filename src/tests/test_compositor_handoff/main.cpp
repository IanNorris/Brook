#include "compositor_handoff.h"
#include "compositor_scanout_lifetime.h"
#include "intel_bdw_present_queue.h"
#include <assert.h>
using namespace brook;
int main(){
    BdwPresentQueue present;
    assert(!present.Admit(0,5) && !present.Take());
    assert(present.Admit(20,1) && present.Holds(20,1) && present.Result(20,1)==0);
    assert(!present.Admit(20,2) && present.Result(21,1)<0);
    assert(present.Take() && !present.Take() && present.Holds(20,1));
    assert(present.Complete(0) && !present.Holds(20,1) && !present.Admit(20,2));
    assert(present.Result(20,1)==1 && present.Result(20,1)<0);
    assert(present.Admit(20,2) && present.Take());
    present.DropOwner(20);
    assert(present.Result(20,2)<0 && !present.Admit(21,3));
    assert(present.Complete(0) && present.Admit(21,3));
    present.DropOwner(20);
    assert(present.Take() && present.Complete(-116) && present.Result(21,3)==117);
    assert(present.Admit(21,4));
    present.DropOwner(21);
    assert(!present.Take() && present.Admit(22,5));

    CompositorHandoff order{};HandoffTicket older{},newer{};
    assert(HandoffAttach(order,20) && HandoffReserve(order,20,7,1,older) && HandoffReserve(order,20,8,1,newer));
    assert(HandoffPresentationAllowed(order,20,older) && HandoffPresentationAllowed(order,20,newer));
    HandoffPresentationCommitted(order,20,newer);
    assert(!HandoffPresentationAllowed(order,20,older) && !HandoffPresentationAllowed(order,20,newer));
    HandoffPresentationCommitted(order,21,older);assert(order.lastPresented==newer.ticket);
    HandoffDetach(order,20);assert(HandoffAttach(order,21) && !order.lastPresented);
    CompositorHandoff h{};HandoffTicket a{},b{},c{};
    assert(!HandoffAttach(h,0) && HandoffAttach(h,10) && !HandoffAttach(h,11));
    assert(HandoffReserve(h,10,1,1,a) && HandoffReserve(h,10,2,1,b));
    assert(!HandoffReserve(h,10,3,1,c));
    assert(!HandoffCancelReservation(h,11,b));
    assert(!HandoffSubmit(h,11,a) && HandoffSubmit(h,10,a));
    assert(!HandoffCancelReservation(h,10,a));
    assert(!HandoffTake(h,a,1,1));
    assert(HandoffComplete(h,a,true));assert(!HandoffTake(h,a,2,1));
    assert(HandoffReserve(h,10,3,1,c));assert(c.ticket!=a.ticket);
    assert(!HandoffComplete(h,a,true));
    assert(HandoffSubmit(h,10,c) && HandoffComplete(h,c,true) && HandoffTake(h,c,3,1));
    HandoffDetach(h,10);assert(h.slots[c.slot].state==HandoffSlotState::Presenting);
    assert(HandoffAttach(h,11));assert(HandoffFinishPresentation(h,c));
    assert(HandoffReserve(h,11,4,1,a) && HandoffSubmit(h,11,a));
    HandoffDetach(h,11);assert(h.slots[a.slot].state==HandoffSlotState::Quarantined);
    assert(HandoffAttach(h,12));assert(!HandoffTake(h,a,4,1));
    assert(HandoffComplete(h,a,false) && h.slots[a.slot].state==HandoffSlotState::Quarantined);
    assert(HandoffComplete(h,a,true) && h.slots[a.slot].state==HandoffSlotState::Free);
    assert(HandoffReserve(h,12,5,2,b) && HandoffSubmit(h,12,b) && HandoffComplete(h,b,true));
    assert(!HandoffTake(h,b,5,3)); // display geometry changed
    h.nextTicket=~uint64_t{0};assert(!HandoffReserve(h,12,1,1,a));
    CompositorHandoff triple{};HandoffTicket d{};
    assert(!HandoffAttach(triple,40,4) && HandoffAttach(triple,40,3));
    assert(HandoffReserve(triple,40,1,1,a) && HandoffReserve(triple,40,2,1,b));
    assert(HandoffReserve(triple,40,3,1,c) && c.slot==2);
    assert(!HandoffReserve(triple,40,4,1,d));
    assert(HandoffSubmit(triple,40,a) && HandoffSubmit(triple,40,c));
    assert(HandoffComplete(triple,c,true) && HandoffTake(triple,c,3,1));
    HandoffPresentationCommitted(triple,40,c);
    assert(!HandoffPresentationAllowed(triple,40,a));
    assert(HandoffFinishPresentation(triple,c) && HandoffReserve(triple,40,4,1,d));
    assert(d.slot==c.slot && !HandoffFind(triple,c) && HandoffSubmit(triple,40,d));
    HandoffDetach(triple,40);
    assert(HandoffAttach(triple,41) && triple.slots[d.slot].state==HandoffSlotState::Quarantined);
    assert(HandoffComplete(triple,d,true) && triple.slots[d.slot].state==HandoffSlotState::Free);
    ScanoutLifetime scan{};uint64_t retired=999,active=0,pending=0;
    assert(!ScanoutPrepare(scan,1,100,false) && ScanoutPrepare(scan,1,100,true));
    assert(ScanoutCancelUnissued(scan)==100 && !scan.pending);
    assert(ScanoutPrepare(scan,1,100,true) && ScanoutIssue(scan));
    assert(!ScanoutComplete(scan,1,999,retired) && retired==999 && scan.pending==100);
    assert(ScanoutComplete(scan,1,100,retired) && !retired && scan.active==100);
    assert(!ScanoutPrepare(scan,1,100,true) && !ScanoutPrepare(scan,2,101,true));
    assert(ScanoutPrepare(scan,1,101,true) && ScanoutIssue(scan));
    assert(!ScanoutCancelUnissued(scan) && !ScanoutComplete(scan,2,101,retired));
    ScanoutUncertain(scan);assert(!ScanoutComplete(scan,1,101,retired));
    assert(!ScanoutPrepare(scan,1,102,true) && scan.active==100 && scan.pending==101);
    assert(ScanoutQuiesced(scan,active,pending) && active==100 && pending==101);
    assert(ScanoutPrepare(scan,2,102,true) && ScanoutIssue(scan) && ScanoutComplete(scan,2,102,retired));
    assert(ScanoutPrepare(scan,2,103,true) && ScanoutIssue(scan) && ScanoutComplete(scan,2,103,retired));
    assert(retired==102 && scan.active==103 && !scan.pending);
    ScanoutUncertain(scan);assert(!ScanoutPrepare(scan,2,104,true));
    assert(ScanoutQuiesced(scan,active,pending) && active==103 && !pending);
}

#include "usb_ecm_tx_slots.h"
#include <cassert>
using Slots=UsbEcmTxSlots;
int main() {
    Slots s{};assert(s.Free(17)==-1);
    for(unsigned i=0;i<16;++i){assert(s.Free(16)==static_cast<int>(i));s.Reserve(i,100+i*2,101+i*2);}
    assert(s.Free(16)==-1 && s.Free(8)==-1);
    assert(s.Complete(101,1,0)==Slots::Result::Error && s.Free(16)==-1);
    assert(s.Complete(100,1,0)==Slots::Result::Intermediate && s.Free(16)==-1);
    assert(s.Complete(100,1,0)==Slots::Result::Error && s.Free(16)==-1);
    assert(s.Complete(101,1,1)==Slots::Result::Error && s.Free(16)==-1);
    assert(s.Complete(101,1,0)==Slots::Result::Finished && s.Free(16)==0);
    s.Reserve(0,900,900);assert(s.Complete(900,1,0)==Slots::Result::Finished);
    assert(s.Complete(900,1,0)==Slots::Result::Unknown);
    assert(s.Complete(102,4,0)==Slots::Result::Error && s.slots[1].active);
    // Repeated generations with mixed regular and two-TD frames preserve
    // ownership until each corresponding final completion.
    s={};
    for(unsigned round=0;round<100;++round){
        for(unsigned i=0;i<16;++i){uint64_t id=1000+round*64+i*2;s.Reserve(i,id,id+(i&1));}
        assert(s.Free(16)==-1);
        for(unsigned i=0;i<16;++i){uint64_t id=1000+round*64+i*2;
            if(i&1)assert(s.Complete(id,1,0)==Slots::Result::Intermediate);
            assert(s.Complete(id+(i&1),1,0)==Slots::Result::Finished);
        }
        assert(s.Free(16)==0);
    }
}

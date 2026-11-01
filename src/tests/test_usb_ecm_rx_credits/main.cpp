#include "usb_ecm_rx_credits.h"
#include <cassert>
#include <cstdint>

int main() {
    UsbEcmRxCredits c{};
    for (unsigned i=0;i<8;++i) {
        assert(c.CanArm(i,8,24));
        c.Arm(i,0x1000+i*16);
    }
    assert(c.outstanding==8);
    assert(!c.CanArm(0,8,24));
    assert(c.Find(0xdead)==-1);
    for (unsigned i=0;i<8;++i) {
        int slot=c.Find(0x1000+i*16);
        assert(slot==static_cast<int>(i));
        c.Complete(static_cast<unsigned>(slot));
        assert(c.Find(0x1000+i*16)==-1); // duplicate completion rejected
        assert(!c.CanArm(i,8,25+i)); // completed frame consumed the credit
    }
    assert(c.outstanding==0);
    assert(!c.CanArm(0,8,32));
    assert(c.CanArm(0,8,31));
    assert(!c.CanArm(1,1,0));
    assert(!c.CanArm(0,9,0));
    assert(!c.CanArm(0,8,33));

    // Reuse slots through many token-ring laps; stale tokens cannot complete
    // a live buffer while it has a different token.
    for (unsigned lap=0;lap<1024;++lap) {
        auto token=uint64_t(0x1000)+(lap%255)*16;
        assert(c.CanArm(0,8,0)); c.Arm(0,token);
        assert(c.Find(token)==0);
        if (lap) assert(c.Find(uint64_t(0x1000)+((lap-1)%255)*16)==-1);
        c.Complete(0);
    }
    return 0;
}

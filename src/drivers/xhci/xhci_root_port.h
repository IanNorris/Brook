#pragma once
#include <stdint.h>
namespace brook {
// xHCI PORTSC mixes persistent fields with write-one actions. In particular,
// PED is RW1CS: copying a read value back can disable the port.
constexpr uint32_t XhciPortPersistent(uint32_t state) {
    return state & ((15u<<5)|(1u<<9)|(3u<<14)|(7u<<25));
}
template<class Io> bool XhciResetRootPort(Io& io,uint64_t timeoutMs) {
    constexpr uint32_t connected=1,enabled=2,reset=1u<<4,resetChanged=1u<<21;
    const auto before=io.read();
    if(before==0xffffffffu || !(before&connected))return false;
    if(before&resetChanged) {
        io.write(XhciPortPersistent(before)|resetChanged);
        const auto cleared=io.read();
        if(cleared==0xffffffffu || (cleared&resetChanged) || !(cleared&connected))return false;
    }
    io.write(XhciPortPersistent(before)|reset);
    const uint64_t start=io.now();
    for(;;) {
        const auto state=io.read();
        if(state==0xffffffffu || !(state&connected))return false;
        if(!(state&reset) && (state&resetChanged)) {
            io.write(XhciPortPersistent(state)|resetChanged);
            const auto ready=io.read();
            return ready!=0xffffffffu && (ready&(connected|enabled|reset))==(connected|enabled);
        }
        if(io.now()-start>=timeoutMs || !io.wait())return false;
    }
}
}

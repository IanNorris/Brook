#pragma once
#include <stdint.h>

namespace brook {
// Emergency UART output never waits for a ticket lock. After one readiness
// timeout, suppress subsequent bytes for this panic so a broken UART cannot
// repeatedly consume the timeout and prevent capture or reset.
struct PanicSerialState { bool failed=false; };
template<class Io>
void PanicSerialWrite(PanicSerialState& state,Io& io,char c,uint32_t pollLimit) {
    if(__atomic_load_n(&state.failed,__ATOMIC_RELAXED))return;
    const auto put=[&](uint8_t byte) {
        for(uint32_t i=0;i<pollLimit;++i) {
            if(io.ready()) {io.write(byte);return true;}
            io.pause();
        }
        return false;
    };
    if((c=='\n' && !put('\r')) || !put(static_cast<uint8_t>(c)))
        __atomic_store_n(&state.failed,true,__ATOMIC_RELAXED);
}
}

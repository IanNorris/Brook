#pragma once
#include <stdint.h>
namespace brook {
struct BdwBcsInitResult {bool attempted=false,resetComplete=false,initComplete=false;uint8_t stage=0;};
// Gen8 BCS execlist startup, following i915's enable_execlists sequence.
// Io must hold exclusive engine/forcewake ownership, ensure the HWS mapping
// and its initialized CSB remain pinned, and serialize ordered MMIO. Once a
// write is attempted, failure leaves hardware state uncertain; the caller
// must quarantine all referenced pages until a proven engine reset.
template<uint32_t EngineBase,class Io> bool BdwInitializeEngineExeclists(Io& io,uint64_t hws) {
    static_assert(EngineBase==0x22000 || EngineBase==0x2000,"unsupported engine");
    constexpr uint32_t hwstam=EngineBase+0x98,mode=EngineBase+0x29c,miMode=EngineBase+0x9c,hwsPga=EngineBase+0x80;
    if(!hws || (hws&4095) || hws>0xfffff000ull || !io.owned() || !io.quiescent() || !io.csbPrepared(hws))return false;
    uint32_t value=0;
    if(!io.writeReg(hwstam,0xffffffffu) || !io.writeReg(mode,0x80008000u) ||
       !io.readReg(mode,value) || value==0xffffffffu || !(value&0x8000u))return false;
    if(!io.writeReg(miMode,0x01000000u) || !io.readReg(miMode,value) ||
       value==0xffffffffu || (value&0x100u))return false;
    if(!io.writeReg(hwsPga,uint32_t(hws)) || !io.readReg(hwsPga,value) || value!=uint32_t(hws))return false;
    // Initializing the memory copy of the producer pointer does not reset the
    // hardware cursor. The backend must prove the cursor and software head
    // agree after engine reset or another documented synchronization sequence.
    return io.owned() && io.quiescent() && io.synchronizeCsb(hws);
}
template<class Io> bool BdwInitializeBcsExeclists(Io& io,uint64_t hws) {
    return BdwInitializeEngineExeclists<0x22000>(io,hws);
}
// Only engine-wide execlist setup. Render default state and workarounds,
// mappings, forcewake and safe retirement remain the owner's responsibility.
template<class Io> bool BdwInitializeRcsExeclists(Io& io,uint64_t hws) {
    return BdwInitializeEngineExeclists<0x2000>(io,hws);
}
}

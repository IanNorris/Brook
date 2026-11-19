#pragma once
#include <stdint.h>
namespace brook {
struct BdwBcsInitResult {bool attempted=false,resetComplete=false,initComplete=false;uint8_t stage=0;};
// Gen8 BCS execlist startup, following i915's enable_execlists sequence.
// Io must hold exclusive engine/forcewake ownership, ensure the HWS mapping
// and its initialized CSB remain pinned, and serialize ordered MMIO. Once a
// write is attempted, failure leaves hardware state uncertain; the caller
// must quarantine all referenced pages until a proven engine reset.
// Diagnostics of the first failed step (hang capture): 1 precondition, 2
// RING_MODE, 3 MI_MODE, 4 HWS_PGA, 5 ownership after setup, 6.. CSB
// synchronization (BdwEngineMmio::synchronizeCsb); value is the last
// register value read at that step.
struct BdwExeclistInitDiag {uint8_t stage=0;uint32_t value=0;};
template<uint32_t EngineBase,class Io> bool BdwInitializeEngineExeclists(Io& io,uint64_t hws,BdwExeclistInitDiag* diag=nullptr) {
    static_assert(EngineBase==0x22000 || EngineBase==0x2000 || EngineBase==0x12000,"unsupported engine");
    constexpr uint32_t hwstam=EngineBase+0x98,mode=EngineBase+0x29c,miMode=EngineBase+0x9c,hwsPga=EngineBase+0x80;
    uint32_t value=0;
    auto fail=[&](uint8_t stage){if(diag){diag->stage=stage;diag->value=value;}return false;};
    if(diag)*diag={};
    if(!hws || (hws&4095) || hws>0xfffff000ull || !io.owned() || !io.quiescent() || !io.csbPrepared(hws))return fail(1);
    if(!io.writeReg(hwstam,0xffffffffu) || !io.writeReg(mode,0x80008000u) ||
       !io.readReg(mode,value) || value==0xffffffffu || !(value&0x8000u))return fail(2);
    if(!io.writeReg(miMode,0x01000000u) || !io.readReg(miMode,value) ||
       value==0xffffffffu || (value&0x100u))return fail(3);
    if(!io.writeReg(hwsPga,uint32_t(hws)) || !io.readReg(hwsPga,value) || value!=uint32_t(hws))return fail(4);
    // Initializing the memory copy of the producer pointer does not reset the
    // hardware cursor. The backend must prove the cursor and software head
    // agree after engine reset or another documented synchronization sequence.
    if(!io.owned() || !io.quiescent())return fail(5);
    return io.synchronizeCsb(hws);
}
template<class Io> bool BdwInitializeBcsExeclists(Io& io,uint64_t hws) {
    return BdwInitializeEngineExeclists<0x22000>(io,hws);
}
// Only engine-wide execlist setup. Render default state and workarounds,
// mappings, forcewake and safe retirement remain the owner's responsibility.
template<class Io> bool BdwInitializeRcsExeclists(Io& io,uint64_t hws) {
    return BdwInitializeEngineExeclists<0x2000>(io,hws);
}
// VCS0 (BSD video engine). Same engine-wide execlist setup; see
// docs/bdw-video-engine.md. Media state and contexts remain the owner's.
template<class Io> bool BdwInitializeVcsExeclists(Io& io,uint64_t hws) {
    return BdwInitializeEngineExeclists<0x12000>(io,hws);
}
}

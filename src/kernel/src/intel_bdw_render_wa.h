#pragma once
#include "intel_bdw_context.h"
namespace brook {
// Masked context-register workarounds emitted into the privileged RCS ring
// after a PIPE_CONTROL barrier and before using a freshly initialized
// context. This matches the Broadwell gen8/BDW context list in i915, with
// the GT3-only SLM fence bit selected explicitly. Execution, forcewake,
// context save and MCR steering remain the hardware owner's responsibility;
// this is not a complete golden render state or the engine/GT workaround set.
inline void BdwBuildRenderContextWa(bool gt3,uint32_t (&commands)[20]) {
    const uint32_t hdc=0x0830u|(gt3?0x4000u:0u);
    const uint32_t words[20]={
        0x11000011, // MI_LOAD_REGISTER_IMM(9)
        0x20c0,0x00800080, // INSTPM_FORCE_ORDERING
        0x209c,0x40004000, // ASYNC_FLIP_PERF_DISABLE
        0xe4f0,0x01200120, // partial shootdown and stall DOP gating
        0x7300,(hdc<<16)|hdc,
        0x7000,0x00040000, // enable HiZ RAW stall optimization
        0x7004,0x00400040, // 4x4 STC optimization disable
        0x7008,0x02800200, // WIZ hashing 16x4
        0xe4f4,0x00010001, // DOP clock gating disable
        0xe184,0x00020002, // sampler power bypass disable
        0 // MI_NOOP for qword alignment
    };
    for(unsigned i=0;i<20;++i)commands[i]=words[i];
}
// Construct the Broadwell render indirect-context workaround batch and attach
// it to an already prepared private context. Both GGTT pages must be separately
// owned, pinned, accessible, cache-published and retained by the caller. This
// prepares memory only; it neither reserves GGTT entries nor submits work.
inline bool BdwBuildRenderIndirectWa(const BdwContext64& c,uint64_t waGgtt,uint64_t scratchGgtt,
                                     uint32_t (&context)[20*1024],uint32_t (&batch)[1024]) {
    constexpr uint64_t limit=uint64_t{1}<<32;
    if(!BdwContext64Valid(c,20*4096) || !waGgtt || !scratchGgtt ||
       (waGgtt&4095) || (scratchGgtt&4095) || waGgtt>limit-4096 || scratchGgtt>limit-4096 ||
       waGgtt==scratchGgtt)return false;
    const auto overlaps=[](uint64_t a,uint64_t as,uint64_t b,uint64_t bs){return a<b+bs && b<a+as;};
    if(overlaps(waGgtt,4096,c.contextGgtt,20*4096) || overlaps(scratchGgtt,4096,c.contextGgtt,20*4096) ||
       overlaps(waGgtt,4096,c.ringGgtt,c.ringBytes) || overlaps(scratchGgtt,4096,c.ringGgtt,c.ringBytes))return false;
    const uintptr_t cp=reinterpret_cast<uintptr_t>(context),bp=reinterpret_cast<uintptr_t>(batch);
    if(cp>~uintptr_t{0}-sizeof(context) || bp>~uintptr_t{0}-sizeof(batch) ||
       (cp<bp+sizeof(batch) && bp<cp+sizeof(context)))return false;
    auto* regs=context+1024;
    if(regs[26]!=0x21c4 || regs[28]!=0x21c8 || regs[27] || regs[29])return false;
    // i915's Broadwell indirect context: disable arbitration, save L3SQC4,
    // enable coherent-line flush, flush DC, restore the register, clear SLM,
    // then re-enable arbitration. Scratch + 256 is reserved exclusively.
    const uint32_t commands[32]={
        0x04000000,
        0x12400002,0xb118,uint32_t(scratchGgtt+256),0,
        0x11000001,0xb118,0x40600000,
        0x7a000004,0x00100020,0,0,0,0,
        0x14c00002,0xb118,uint32_t(scratchGgtt+256),0,
        0x7a000004,0x08304000,0xd0,0,0,0,
        0x04000001,0,0,0,0,0,0,0
    };
    for(unsigned i=0;i<1024;++i)batch[i]=i<32?commands[i]:0;
    regs[27]=uint32_t(waGgtt)|2u; // two 64-byte cachelines
    regs[29]=0x17u<<6; // Broadwell indirect context offset
    return true;
}
}

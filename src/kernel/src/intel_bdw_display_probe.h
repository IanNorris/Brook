#pragma once
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_cursor.h"
namespace brook {
// Raw point-in-time display register evidence only. A powered-down display
// domain may read as zero, and firmware may change it later: neither this
// snapshot nor a disabled plane is an ownership or GGTT allocation grant.
struct BdwDisplaySnapshot {
    static constexpr unsigned PrimaryOffset=10, RegisterCount=11;
    uint32_t regs[3][RegisterCount]{};
};

inline bool BdwDisplayUsesGop(const BdwDisplaySnapshot& display) {
    const auto& primary=display.regs[0];
    // Exact linear XRGB8888 mode and base inherited from Dell's GOP. No
    // other display plane may reference the private GGTT command slots.
    if(primary[0]!=0x98000000u || primary[1]!=12800)return false;
    for(unsigned pipe=0;pipe<3;++pipe)
        for(unsigned reg=pipe?0:2;reg<BdwDisplaySnapshot::RegisterCount;++reg)
            if(display.regs[pipe][reg])return false;
    return true;
}

// As BdwDisplayUsesGop, but the primary surface and its live value may be
// any surface accept() admits (the GOP or a mapped kernel flip buffer).
// registers limits the checked set (the per-job check historically ignores
// the trailing primary tile offset of pipes B and C).
template<class Accept> bool BdwDisplayCompatible(const BdwDisplaySnapshot& display,Accept accept,
                                                 unsigned registers=BdwDisplaySnapshot::RegisterCount,
                                                 bool (*cursorBaseOwned)(uint32_t)=nullptr) {
    const auto& primary=display.regs[0];
    if(registers<4 || registers>BdwDisplaySnapshot::RegisterCount)return false;
    if(primary[0]!=0x98000000u || primary[1]!=12800 || !accept(primary[2]) || !accept(primary[3]))return false;
    for(unsigned pipe=0;pipe<3;++pipe)
        for(unsigned reg=pipe?0:4;reg<registers;++reg){
            if(!pipe && reg==7 && registers>=10 && cursorBaseOwned){
                const uint32_t control=primary[7], base=primary[8], live=primary[9];
                // Arm and disable take effect at vblank. Both kernel-owned
                // bases may still be fetched during the transition.
                if((control!=0 && control!=BdwCursorRegs::Argb64) ||
                   (control && !base) || (base && !cursorBaseOwned(base)) ||
                   (live && !cursorBaseOwned(live)))return false;
                reg=9;
            }else if(display.regs[pipe][reg])return false;
        }
    return true;
}

template<class Io> bool BdwDisplayCriticalRead(Io& io,BdwDisplaySnapshot& out) {
    constexpr uint64_t base=0xf6070000ull;
    // Primary control/stride/surface/live, sprite control/surface/live, and
    // cursor control/base/live. Broadwell pipes A, B and C are 0x1000 apart.
    constexpr uint32_t offsets[]={0x180,0x188,0x19c,0x1ac,0x280,0x29c,0x2ac,0x80,0x84,0xac,0x1a4};
    static_assert(sizeof(offsets)/sizeof(*offsets)==BdwDisplaySnapshot::RegisterCount);
    BdwDisplaySnapshot first{},second{};
    for(unsigned pipe=0;pipe<3;++pipe) {
        DellI2cCriticalEvidence e{};
        if(!io.evidence(base+pipe*4096,e) || !BdwGttEvidenceValid(e))return false;
    }
    for(unsigned pass=0;pass<2;++pass)for(unsigned pipe=0;pipe<3;++pipe)
        for(unsigned reg=0;reg<BdwDisplaySnapshot::RegisterCount;++reg) {
            uint32_t& value=pass?second.regs[pipe][reg]:first.regs[pipe][reg];
            if(!io.read32(base+pipe*4096+offsets[reg],value) || value==0xffffffffu)return false;
        }
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned reg=0;reg<BdwDisplaySnapshot::RegisterCount;++reg)
        if(first.regs[pipe][reg]!=second.regs[pipe][reg])return false;
    out=first;return true;
}
}

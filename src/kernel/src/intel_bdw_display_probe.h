#pragma once
#include "intel_bdw_gtt_probe.h"
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

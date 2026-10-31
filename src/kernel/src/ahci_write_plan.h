#pragma once
#include "ahci_read_plan.h"
#include "ata_identify.h"

namespace brook {
// Pure command descriptions only: they cannot submit to a controller. The
// read-only driver deliberately does not accept either of these opcodes.
inline bool AhciPlanWrite(AhciReadPlan& out, uint64_t lba, uint32_t sectors,
                          uint64_t diskSectors, uint64_t dma, bool dma64) {
    if(!AhciPlanRead(out,lba,sectors,diskSectors,dma,dma64))return false;
    out.fis[2]=0x35; // WRITE DMA EXT
    out.commandFlags |= 1u<<6; // host to device
    return true;
}

// Require an explicitly advertised, valid FLUSH CACHE EXT capability before
// admitting durable writes. The 20-byte FIS has no associated data or PRDT.
inline bool AtaSupportsFlushExt(const uint8_t* identify, uint32_t bytes) {
    if(!identify || bytes<512)return false;
    return (AtaIdentifyWord(identify,83)&0xe000u)==0x6000u;
}
inline bool AhciPlanFlush(AhciReadPlan& out, bool flushExtSupported) {
    out={};
    if(!flushExtSupported)return false;
    out.commandFlags=5;
    out.fis[0]=0x27;out.fis[1]=0x80;out.fis[2]=0xea; // FLUSH CACHE EXT
    return true;
}
} // namespace brook

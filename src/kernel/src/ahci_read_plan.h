#pragma once
#include <stdint.h>

namespace brook {
// A pure description of a single-slot, non-NCQ AHCI read. Materializing and
// submitting it is a separate driver operation. No write/flush command exists
// in this interface.
struct AhciReadPlan {
    uint8_t fis[20]{};
    uint16_t commandFlags = 0; // CFL=5; write bit clear
    uint16_t prdtEntries = 0;
    uint32_t dmaLow = 0, dmaHigh = 0, byteCountMinusOne = 0;
};

inline bool AhciReadPlanDma(AhciReadPlan& out, uint64_t dma, uint32_t bytes, bool dma64) {
    if (!bytes || (bytes & 1u) || bytes > 65536 || (dma & 1u) || dma > ~uint64_t{0} - (bytes - 1)) return false;
    if (!dma64 && dma + bytes - 1 > 0xffffffffull) return false;
    out.commandFlags=5;out.prdtEntries=1;out.dmaLow=uint32_t(dma);out.dmaHigh=uint32_t(dma>>32);
    out.byteCountMinusOne=bytes-1;
    out.fis[0]=0x27;out.fis[1]=0x80; // host-to-device register FIS, command bit
    return true;
}

inline bool AhciPlanIdentify(AhciReadPlan& out, uint64_t dma, bool dma64) {
    out={};if(!AhciReadPlanDma(out,dma,512,dma64))return false;
    out.fis[2]=0xec; // IDENTIFY DEVICE
    return true;
}

inline bool AhciPlanRead(AhciReadPlan& out, uint64_t lba, uint32_t sectors,
                         uint64_t diskSectors, uint64_t dma, bool dma64) {
    out={};
    constexpr uint64_t lbaLimit=1ull<<48;
    if(!sectors || sectors>128 || lba>=lbaLimit || sectors>lbaLimit-lba ||
       lba>=diskSectors || sectors>diskSectors-lba) return false;
    if(!AhciReadPlanDma(out,dma,sectors*512,dma64))return false;
    out.fis[2]=0x25; // READ DMA EXT
    out.fis[4]=uint8_t(lba);out.fis[5]=uint8_t(lba>>8);out.fis[6]=uint8_t(lba>>16);
    out.fis[7]=0x40;out.fis[8]=uint8_t(lba>>24);out.fis[9]=uint8_t(lba>>32);out.fis[10]=uint8_t(lba>>40);
    out.fis[12]=uint8_t(sectors);out.fis[13]=uint8_t(sectors>>8);
    return true;
}
} // namespace brook

#pragma once
#include <stdint.h>
namespace brook {
// xHCI 1.22 §7.1: byte ownership write leaves the BIOS semaphore untouched.
// Preflight the entire bounded capability list before taking ownership.
template<class Io> bool XhciAcquireFirmwareOwnership(Io& io,uint32_t first,uint32_t bytes,
                                                      uint32_t& legacy) {
    legacy=0;if(bytes<8)return false;
    uint32_t offset=first;
    for(unsigned hops=0;offset && hops<256;++hops) {
        if((offset&3) || offset<0x20 || offset>bytes-4)return false;
        const uint32_t header=io.read(offset);
        if(header==0xffffffffu)return false;
        if((header&255)==1) {
            if(legacy || offset>bytes-8)return false;
            legacy=offset;
        }
        const uint32_t delta=((header>>8)&255)*4;
        if(!delta){offset=0;break;}
        if(delta>bytes-offset-4)return false;
        offset+=delta;
    }
    if(offset)return false;
    if(!legacy)return true;
    io.writeByte(legacy+3,static_cast<uint8_t>((io.read(legacy)>>24)|1));
    bool acquired=false;
    for(unsigned tries=0;tries<1000000;++tries) {
        const auto state=io.read(legacy);
        if((state&((1u<<24)|(1u<<16)))==(1u<<24)){acquired=true;break;}
        if(!io.wait())break;
    }
    if(!acquired)return false;
    // Preserve reserved fields; disable the five SMI enables and acknowledge
    // ownership/PCI/BAR W1C events. Do not write reserved-zero fields.
    constexpr uint32_t reserved=(7u<<1)|(255u<<5)|(7u<<17);
    io.write(legacy+4,(io.read(legacy+4)&reserved)|(7u<<29));
    return !(io.read(legacy+4)&((1u<<0)|(1u<<4)|(7u<<13)));
}
}

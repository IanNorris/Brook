#pragma once
#include <stdint.h>
namespace brook {
struct XhciMsiLayout { uint8_t upper=0,data=0,mask=0;uint16_t enabled=0; };
inline bool XhciMsiRegisters(uint32_t offset,uint16_t control,XhciMsiLayout& out) {
    if(offset<0x40 || (offset&3) || offset>0xe8)return false;
    const bool wide=control&(1u<<7),masked=control&(1u<<8);
    out.upper=wide?offset+8:0;out.data=offset+(wide?12:8);
    out.mask=masked?offset+(wide?16:12):0;
    out.enabled=(control&~0x70u)|1; // one message, enable MSI
    return true;
}
inline uint32_t XhciScratchpadCount(uint32_t value) {
    return (((value>>21)&31)<<5)|((value>>27)&31);
}
}

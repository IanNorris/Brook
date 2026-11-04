#pragma once
#include <stdint.h>
#include "../../shared/update/payload.h"

namespace brook {
// Identify only an explicitly scoped segment-zero endpoint 00:02.0. This
// generic parser alone is not admission for MMIO or DMA.
struct DmarGpuUnit { bool found; uint64_t registerBase; };
inline uint16_t DmarU16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1])<<8); }
inline uint32_t DmarU32(const uint8_t* p) { return uint32_t(DmarU16(p)) | (uint32_t(DmarU16(p+2))<<16); }
inline uint64_t DmarU64(const uint8_t* p) { return uint64_t(DmarU32(p)) | (uint64_t(DmarU32(p+4))<<32); }
inline DmarGpuUnit DmarExplicitDellGpuUnit(const uint8_t* table,uint32_t bytes) {
    if (!table || bytes<48 || bytes>65536 || table[0]!='D' || table[1]!='M' || table[2]!='A' || table[3]!='R' || DmarU32(table+4)!=bytes) return {};
    uint8_t sum=0;for(uint32_t i=0;i<bytes;++i)sum=uint8_t(sum+table[i]);
    if(sum)return {};
    DmarGpuUnit result{};
    for(uint32_t pos=48;pos<bytes;) {
        if(bytes-pos<4)return {};
        const uint16_t type=DmarU16(table+pos), length=DmarU16(table+pos+2);
        if(length<4 || length>bytes-pos)return {};
        if(type==0) {
            if(length<16 || (table[pos+4]&~1u))return {};
            const uint16_t segment=DmarU16(table+pos+6);
            const uint64_t base=DmarU64(table+pos+8);
            bool matched=false;
            for(uint32_t off=16;off<length;) {
                if(length-off<6)return {};
                const uint8_t scopeLength=table[pos+off+1];
                if(scopeLength<8 || scopeLength>length-off || ((scopeLength-6)&1))return {};
                if(segment==0 && table[pos+off]==1 && table[pos+off+5]==0 && scopeLength==8 &&
                   table[pos+off+6]==2 && table[pos+off+7]==0) {
                    if(matched || result.found)return {};
                    matched=true;
                }
                off+=scopeLength;
            }
            if(matched) {
                // An include-all unit is not a dedicated explicit owner.
                if((table[pos+4]&1) || !base || (base&4095))return {};
                result={true,base};
            }
        }
        pos+=length;
    }
    return result;
}

// Narrow diagnostic admission for the reviewed XPS 9343 firmware table only.
// The caller must additionally verify that the ACPI root has exactly one DMAR
// and that the expected Dell GPU is present. The table must be a private,
// immutable copy for the whole call. No live register or DMA access is
// authorized by this function; those require separate cache and ownership gates.
inline DmarGpuUnit DmarAuditedDellGpuUnit(const uint8_t* table,uint32_t bytes) {
    static constexpr uint8_t digest[32]={0xe7,0x96,0x9f,0x2c,0x3c,0x98,0xce,0x25,0xac,0x6c,0xd0,0x7a,0xab,0x02,0xb6,0x10,0x55,0x72,0xcd,0x80,0xc9,0x1d,0x83,0xc9,0xb2,0xe2,0x15,0x3a,0xfd,0x4e,0xf5,0x8e};
    if(!table || bytes!=212)return {};
    uint8_t actual[32];BrookUpdateHash(table,bytes,actual);
    if(!BrookUpdateEqual(actual,digest,32))return {};
    const auto unit=DmarExplicitDellGpuUnit(table,bytes);
    return unit.found && unit.registerBase==0xfed90000 ? unit : DmarGpuUnit{};
}
}

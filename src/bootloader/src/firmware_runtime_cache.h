#pragma once
#include "boot_protocol/efi_runtime_cache.h"
#include "boot_protocol/efi_runtime_mtrr.h"
#include "../../kernel/src/memory/kernel_alias_probe.h"
namespace brook { namespace bootloader {
struct FirmwareRuntimeCache {
    RuntimeCacheFingerprint fingerprint{};
    uint64_t pat=0;
    uint64_t mtrr=0;
};
inline uint64_t FirmwareRuntimeMtrrFingerprint() {
    uint32_t a=0,b=0,c=0,d=0;
    __asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d):"c"(0));
    if(b!=0x756e6547 || d!=0x49656e69 || c!=0x6c65746e)return 0;
    a=1;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d):"c"(0));
    if(!(d&(1u<<12)))return 0;
    a=0x80000000u;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d):"c"(0));
    if(a<0x80000008u)return 0;
    a=0x80000008u;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d):"c"(0));
    const uint32_t bits=a&255;
    auto msr=[](uint32_t id){uint32_t lo=0,hi=0;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(id));return uint64_t(lo)|(uint64_t(hi)<<32);};
    const uint64_t cap=msr(0xfe),defaults=msr(0x2ff);const uint32_t count=cap&255;
    if(count>32)return 0;
    uint64_t base[32]{},mask[32]{},fixed[11]{};
    for(uint32_t i=0;i<count;++i){base[i]=msr(0x200+2*i);mask[i]=msr(0x201+2*i);}
    if(cap&(1ull<<8)){fixed[0]=msr(0x250);fixed[1]=msr(0x258);fixed[2]=msr(0x259);
        for(unsigned i=0;i<8;++i)fixed[i+3]=msr(0x268+i);}
    return RuntimeMtrrFingerprint(bits,cap,defaults,base,mask,fixed,count);
}
inline FirmwareRuntimeCache CaptureFirmwareRuntimeCache(const MemoryDescriptor* map,uint32_t count) {
    FirmwareRuntimeCache out{};uint32_t a=1,b=0,c=0,d=0;
    __asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d):"c"(0));
    if(!(d&(1u<<16)))return out; // CPUID.01H:EDX.PAT
    uint64_t root=0;__asm__ volatile("mov %%cr3,%0":"=r"(root));root&=0x000ffffffffff000ull;
    struct Tables {
        const MemoryDescriptor* map;uint32_t count;
        bool readEntry(uint64_t page,uint16_t index,uint64_t& value) {
            if(!page || (page&4095) || index>=512)return false;
            for(uint32_t i=0;i<count;++i){const auto& m=map[i];
                if(m.type==MemoryType::Mmio || m.type==MemoryType::Unusable || !m.pageCount ||
                   m.pageCount>(~uint64_t{0}-m.physicalStart)/4096)continue;
                const uint64_t end=m.physicalStart+m.pageCount*4096;
                if(page>=m.physicalStart && page<=end && end-page>=4096){
                    value=reinterpret_cast<const uint64_t*>(page)[index];return true;
                }
            }
            return false;
        }
    } tables{map,count};
    auto probe=[&](uint64_t address,uint64_t& physical,uint8_t& pat){KernelAliasMapping m{};
        if(!InspectKernelAlias(tables,root,address,m) || !m.present)return false;
        physical=m.physical;pat=m.patIndex;return true;};
    out.fingerprint=FingerprintRuntimeCache(map,count,probe);
    if(!out.fingerprint.valid)return out;
    uint32_t low=0,high=0;__asm__ volatile("rdmsr":"=a"(low),"=d"(high):"c"(0x277));
    out.pat=uint64_t(low)|(uint64_t(high)<<32);
    const auto first=FirmwareRuntimeMtrrFingerprint(),second=FirmwareRuntimeMtrrFingerprint();
    if(first && first==second)out.mtrr=first;
    return out;
}
}}

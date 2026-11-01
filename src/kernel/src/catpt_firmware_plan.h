#pragma once
#include <stdint.h>

namespace brook {
// Offline WPT firmware inventory only: no physical addresses, MMIO, firmware
// execution or claim that INSTANCE blocks share DRAM loading semantics.
// Deliberately caps the plan at 256 blocks, unlike the Python inventory. The
// local plan uses about 4 KiB of stack; check stack budget before kernel use.
enum class CatptRegion : uint8_t { Iram=1, Dram=2, Instance=3 };
struct CatptBlockPlan { uint32_t source, size, offset; CatptRegion region; };
struct CatptFirmwarePlan {
    static constexpr uint32_t MaxBlocks=256;
    uint32_t imageBytes=0,modules=0,blocks=0;
    CatptBlockPlan block[MaxBlocks]{};
};
inline uint16_t CatptGet16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
inline uint32_t CatptGet32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
inline bool CatptSignature(const uint8_t* p) { return p[0]=='$' && p[1]=='S' && p[2]=='S' && p[3]=='T'; }

// The image must remain immutable for the lifetime of the returned plan.
// Failure leaves the caller's output untouched and performs no callbacks.
inline bool CatptBuildPlan(const uint8_t* image,uint32_t bytes,CatptFirmwarePlan& output) {
    constexpr uint32_t Header=32,Module=28,Block=16,MaxImage=16u*1024*1024;
    if(!image || bytes<Header || bytes>MaxImage || !CatptSignature(image) ||
       CatptGet32(image+4)!=bytes-Header || CatptGet32(image+12)!=254)return false;
    const uint32_t count=CatptGet32(image+8);
    if(!count || count>256 || count>(bytes-Header)/Module)return false;
    CatptFirmwarePlan plan{};plan.imageBytes=bytes;plan.modules=count;
    uint32_t at=Header;uint32_t seen=0;
    for(uint32_t m=0;m<count;++m) {
        if(at>bytes || bytes-at<Module || !CatptSignature(image+at))return false;
        const uint32_t size=CatptGet32(image+at+4),blocks=CatptGet32(image+at+8);
        const uint32_t id=CatptGet16(image+at+14);
        if(id>15 || (seen&(1u<<id)))return false;
        seen|=1u<<id;at+=Module;
        if(size>bytes-at || blocks>size/Block)return false;
        const uint32_t end=at+size;
        for(uint32_t i=0;i<blocks;++i) {
            if(at>end || end-at<Block || plan.blocks==CatptFirmwarePlan::MaxBlocks)return false;
            const uint32_t kind=CatptGet32(image+at),length=CatptGet32(image+at+4),dest=CatptGet32(image+at+8);
            at+=Block;
            if(kind<1 || kind>3 || !length || length>end-at)return false;
            const uint32_t limit=kind==1?0x50000u:0xa0000u;
            if(dest>limit || length>limit-dest)return false;
            for(uint32_t j=0;j<plan.blocks;++j) {
                const auto& prior=plan.block[j];
                const bool sameRegion=(kind==1)==(prior.region==CatptRegion::Iram);
                if(sameRegion && dest<prior.offset+prior.size && prior.offset<dest+length)return false;
            }
            plan.block[plan.blocks++]={at,length,dest,static_cast<CatptRegion>(kind)};
            at+=length;
        }
        if(at!=end)return false;
    }
    if(at!=bytes)return false;
    output=plan;return true;
}
}

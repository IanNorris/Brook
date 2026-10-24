#pragma once
#include "signal_fp_state.h"
#include "user_copy.h"

namespace brook {
inline bool SignalFpCapabilities(uint64_t& features,uint32_t& mxcsrMask) {
    uint32_t lo,hi;
    __asm__ volatile("xgetbv":"=a"(lo),"=d"(hi):"c"(0));
    features=lo;
    if(hi || (features!=3 && features!=7))return false;
    if(features&4) {
        uint32_t a,b,c,d;
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0xd),"c"(2));
        if(a!=256 || b!=576)return false;
    }
    // This samples only the hardware mask. Actual interrupted state always
    // comes from the assembly entry image, before any kernel vector use.
    alignas(16) uint8_t sample[512];
    __asm__ volatile("fxsave64 %0":"=m"(sample));
    mxcsrMask=SignalFpGet<uint32_t>(sample,28);
    if(!mxcsrMask)mxcsrMask=0xffbf;
    return true;
}
inline bool SignalFpSaveToUser(uint64_t address,const uint8_t* entry) {
    uint64_t features;uint32_t mxcsrMask;
    if(!entry || !SignalFpCapabilities(features,mxcsrMask))return false;
    SignalFpImage image;
    return SignalFpExport(image,entry,features,mxcsrMask) &&
           CopyToUser(address,image.data,SignalFpStateSize(features)+4);
}
inline bool SignalFpReadFromUser(SignalFpImage& image,uint64_t address) {
    if(!address) {
        SignalFpReset(image.data);
        return true; // XRSTOR init state, including MXCSR's special load rule.
    }
    uint64_t features;uint32_t mxcsrMask;
    if(!SignalFpCapabilities(features,mxcsrMask))return false;
    SignalFpImage input{};
    if(!CopyFromUser(input.data,address,512))return false;
    size_t bytes=512;
    size_t size=0;
    if(SignalFpExtendedMetadata(input.data,features,size)) {
        if(address>0x0000800000000000ULL-(size+4))return false;
        bytes=size+4;
        // Discovery comes before extended-state validation. A readable bad
        // trailer is legacy fallback; an unreadable trailer is still a fault.
        uint32_t trailer=0;
        if(!CopyFromUser(&trailer,address+size,4))return false;
        SignalFpPut<uint32_t>(input.data,size,trailer);
        if(trailer==SIGNAL_FP_MAGIC2 &&
           !CopyFromUser(input.data+512,address+512,size-512))return false;
    }
    return SignalFpImport(image,input.data,bytes,features,mxcsrMask);
}
}

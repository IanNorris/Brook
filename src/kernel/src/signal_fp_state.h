#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef BROOK_HOST_TEST
#include <cstring>
#else
#include "string.h"
#endif

namespace brook {
constexpr uint32_t SIGNAL_FP_MAGIC1=0x46505853, SIGNAL_FP_MAGIC2=0x46505845;
constexpr size_t SIGNAL_FP_MAX_BYTES=836, SIGNAL_FP_EXTRA=63+SIGNAL_FP_MAX_BYTES;
struct alignas(64) SignalFpImage { uint8_t data[1088]; };
template<class T> inline T SignalFpGet(const uint8_t* p,size_t offset) {
    T value;memcpy(&value,p+offset,sizeof(value));return value;
}
template<class T> inline void SignalFpPut(uint8_t* p,size_t offset,T value) {
    memcpy(p+offset,&value,sizeof(value));
}
inline size_t SignalFpStateSize(uint64_t features) { return (features&4)?832:576; }
inline void SignalFpReset(uint8_t* image) {
    memset(image,0,1088);
    SignalFpPut<uint16_t>(image,0,0x037f);
    SignalFpPut<uint32_t>(image,24,0x1f80);
}
inline uint64_t SignalFpUserAddress(uint64_t frame,size_t bytes) {
    return (frame+bytes+63)&~uint64_t{63};
}
// Only architecturally defined legacy fields. In particular, never copy x87
// slot padding, legacy reserved bytes or the kernel slot's unused tail.
inline void SignalFpCopyLegacy(uint8_t* dst,const uint8_t* src,uint64_t present,
                               uint32_t mxcsrMask) {
    SignalFpPut<uint16_t>(dst,0,0x037f);
    if(present&1) {
        memcpy(dst,src,5);memcpy(dst+6,src+6,18);
        for(size_t i=0;i<8;++i)memcpy(dst+32+i*16,src+32+i*16,10);
    }
    // XSAVE writes MXCSR independently of the SSE in-use bit when SSE/AVX
    // are requested. Do not mistake an init XMM component for an absent MXCSR.
    memcpy(dst+24,src+24,4);
    SignalFpPut<uint32_t>(dst,28,mxcsrMask);
    if(present&2)memcpy(dst+160,src+160,256);
}
inline bool SignalFpExport(SignalFpImage& out,const uint8_t* entry,
                            uint64_t features,uint32_t mxcsrMask) {
    if(features!=3 && features!=7)return false;
    const uint64_t present=SignalFpGet<uint64_t>(entry,512)&features;
    const uint32_t mxcsr=SignalFpGet<uint32_t>(entry,24);
    if(mxcsr&~mxcsrMask)return false;
    memset(&out,0,sizeof(out));
    SignalFpCopyLegacy(out.data,entry,present,mxcsrMask);
    if(present&4)memcpy(out.data+576,entry+576,256);
    // Legacy consumers may edit these fields without knowing XSAVE headers.
    SignalFpPut<uint64_t>(out.data,512,present|3);
    const size_t size=SignalFpStateSize(features);
    SignalFpPut<uint32_t>(out.data,464,SIGNAL_FP_MAGIC1);
    SignalFpPut<uint32_t>(out.data,468,static_cast<uint32_t>(size+4));
    SignalFpPut<uint64_t>(out.data,472,features);
    SignalFpPut<uint32_t>(out.data,480,static_cast<uint32_t>(size));
    SignalFpPut<uint32_t>(out.data,size,SIGNAL_FP_MAGIC2);
    return true;
}
// Snapshot validation only: caller copies from user memory first, and commits
// the resulting image to the entry wrapper only after all context checks pass.
inline bool SignalFpExtendedMetadata(const uint8_t* input,uint64_t features,size_t& size) {
    size=SignalFpGet<uint32_t>(input,480);
    return SignalFpGet<uint32_t>(input,464)==SIGNAL_FP_MAGIC1 && size>=576 &&
           size<=SignalFpStateSize(features) && SignalFpGet<uint32_t>(input,468)>=size+4;
}
inline bool SignalFpImport(SignalFpImage& out,const uint8_t* input,size_t bytes,
                            uint64_t features,uint32_t mxcsrMask) {
    if((features!=3 && features!=7) || bytes<512)return false;
    if(SignalFpGet<uint32_t>(input,24)&~mxcsrMask)return false;
    uint64_t present=3;
    size_t discoveredSize=0;
    bool extended=SignalFpExtendedMetadata(input,features,discoveredSize);
    if(extended) {
        const size_t size=discoveredSize;
        if(bytes<size+4)return false;
        // Linux falls back to the legacy FXSAVE state when the extension's
        // trailer marker is absent. Never interpret trailing bytes as XSAVE.
        if(SignalFpGet<uint32_t>(input,size)!=SIGNAL_FP_MAGIC2)extended=false;
    }
    if(extended) {
        const size_t size=SignalFpGet<uint32_t>(input,480);
        const uint64_t advertised=SignalFpGet<uint64_t>(input,472);
        present=SignalFpGet<uint64_t>(input,512);
        if((advertised&~features) || (present&~advertised) ||
           ((present&4) && size<832) || SignalFpGet<uint64_t>(input,520))return false;
        for(size_t i=528;i<576;++i)if(input[i])return false;
    }
    memset(&out,0,sizeof(out));
    SignalFpCopyLegacy(out.data,input,present,mxcsrMask);
    if(extended && (present&4))memcpy(out.data+576,input+576,256);
    SignalFpPut<uint64_t>(out.data,512,present);
    return true;
}
}

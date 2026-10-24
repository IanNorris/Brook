#include <cassert>
#include <cstdio>
#define BROOK_HOST_TEST 1
#include "../../kernel/src/signal_fp_state.h"
using namespace brook;
int main() {
    SignalFpImage raw,out,restored;
    memset(&raw,0xa5,sizeof(raw));
    SignalFpPut<uint64_t>(raw.data,512,7);
    SignalFpPut<uint32_t>(raw.data,24,0x1fa0);
    assert(SignalFpExport(out,raw.data,7,0xffff));
    assert(SignalFpGet<uint32_t>(out.data,24)==0x1fa0);
    assert(SignalFpGet<uint64_t>(out.data,512)==7);
    assert(out.data[5]==0);
    for(size_t slot=0;slot<8;++slot)
        for(size_t i=10;i<16;++i)assert(out.data[32+slot*16+i]==0);
    for(size_t i=416;i<464;++i)assert(out.data[i]==0);
    for(size_t i=484;i<512;++i)assert(out.data[i]==0);
    for(size_t i=520;i<576;++i)assert(out.data[i]==0);
    for(size_t i=836;i<sizeof(out.data);++i)assert(out.data[i]==0);
    assert(SignalFpGet<uint32_t>(out.data,464)==SIGNAL_FP_MAGIC1);
    assert(SignalFpGet<uint32_t>(out.data,468)==836);
    assert(SignalFpGet<uint64_t>(out.data,472)==7);
    assert(SignalFpGet<uint32_t>(out.data,480)==832);
    assert(SignalFpGet<uint32_t>(out.data,832)==SIGNAL_FP_MAGIC2);
    assert(SignalFpImport(restored,out.data,836,7,0xffff));
    assert(restored.data[400]==0xa5 && restored.data[816]==0xa5);
    for(size_t i=464;i<512;++i)assert(restored.data[i]==0);
    auto bad=out;SignalFpPut<uint32_t>(bad.data,24,0x10000);
    assert(!SignalFpImport(restored,bad.data,836,7,0xffff));
    bad=out;SignalFpPut<uint64_t>(bad.data,512,8);
    assert(!SignalFpImport(restored,bad.data,836,7,0xffff));
    bad=out;SignalFpPut<uint64_t>(bad.data,520,1ULL<<63);
    assert(!SignalFpImport(restored,bad.data,836,7,0xffff));
    for(size_t i=528;i<576;++i) {
        bad=out;bad.data[i]=1;
        assert(!SignalFpImport(restored,bad.data,836,7,0xffff));
    }
    bad=out;bad.data[832]^=1;
    assert(SignalFpImport(restored,bad.data,836,7,0xffff));
    assert(SignalFpGet<uint64_t>(restored.data,512)==3);
    for(size_t i=576;i<832;++i)assert(restored.data[i]==0);
    assert(!SignalFpImport(restored,out.data,835,7,0xffff));
    // Unsupported extension discovery falls back to the legacy image.
    assert(SignalFpImport(restored,out.data,512,3,0xffff));
    assert(SignalFpGet<uint64_t>(restored.data,512)==3);
    bad=out;SignalFpPut<uint32_t>(bad.data,468,0xffffffff);
    assert(SignalFpImport(restored,bad.data,836,7,0xffff));
    bad=out;SignalFpPut<uint32_t>(bad.data,480,9000);
    assert(SignalFpImport(restored,bad.data,512,7,0xffff));
    bad=out;bad.data[832]^=1;SignalFpPut<uint32_t>(bad.data,24,0x10000);
    assert(!SignalFpImport(restored,bad.data,836,7,0xffff));
    // Non-XSAVE legacy input initializes the AVX upper halves.
    bad=out;SignalFpPut<uint32_t>(bad.data,464,0);
    assert(SignalFpImport(restored,bad.data,512,7,0xffff));
    assert(SignalFpGet<uint64_t>(restored.data,512)==3);
    for(size_t i=576;i<832;++i)assert(restored.data[i]==0);
    // Init-state components must never leak poisoned XSAVE slot bytes.
    SignalFpPut<uint64_t>(raw.data,512,0);
    SignalFpPut<uint32_t>(raw.data,24,0x1f80);
    assert(SignalFpExport(out,raw.data,7,0xffff));
    assert(SignalFpGet<uint16_t>(out.data,0)==0x037f);
    assert(SignalFpGet<uint64_t>(out.data,512)==3);
    for(size_t i=32;i<416;++i)assert(out.data[i]==0);
    for(size_t i=576;i<832;++i)assert(out.data[i]==0);
    assert(SignalFpExport(out,raw.data,3,0xffbf));
    assert(SignalFpGet<uint32_t>(out.data,480)==576);
    assert(SignalFpGet<uint32_t>(out.data,576)==SIGNAL_FP_MAGIC2);
    assert(SignalFpImport(restored,out.data,580,3,0xffbf));
    for(uint64_t base=0x10000;base<0x10080;++base)
        assert(!(SignalFpUserAddress(base,440)&63));
    memset(&out,0xa5,sizeof(out));SignalFpReset(out.data);
    assert(SignalFpGet<uint16_t>(out.data,0)==0x037f);
    assert(SignalFpGet<uint32_t>(out.data,24)==0x1f80);
    assert(SignalFpGet<uint64_t>(out.data,512)==0);
    puts("signal FP normalization, privacy, legacy, AVX and malformed-state checks passed");
}

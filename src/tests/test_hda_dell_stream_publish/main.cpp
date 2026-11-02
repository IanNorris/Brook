#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_stream_publish.h"
using namespace brook;
struct Device {
    uint8_t cpu[4096]{},device[4096]{};uint64_t cursor=0,advance=0;unsigned writes=0,flushes=0,samples=0,fences=0;
    unsigned failSampleAt=0,failWriteAt=0,failFlushAt=0;bool failFence=false,backward=false;
    bool sample(uint64_t& p){++samples;if(samples==failSampleAt)return false;p=backward && samples==2?cursor-1:cursor;return true;}
    bool write(uint32_t at,const uint8_t* src,uint32_t n){++writes;if(writes==failWriteAt)return false;
        if(src)std::memcpy(cpu+at,src,n);else std::memset(cpu+at,0,n);return true;}
    bool publish(uint32_t at,uint32_t n){++flushes;if(flushes==failFlushAt)return false;
        const uint32_t first=at&~63u,last=(at+n+63)&~63u;
        std::memcpy(device+first,cpu+first,last-first);cursor+=advance;return true;}
    bool fence(){++fences;return !failFence;}
};
int main(){
    assert(HdaStreamSafeWindow(100,0,256,400,4096,64,128));
    assert(!HdaStreamSafeWindow(100,0,224,400,4096,64,128));
    assert(!HdaStreamSafeWindow(100,0,256,4070,4096,64,128));
    assert(HdaStreamSafeWindow(4000,0,4160,4256,4096,64,128));
    assert(!HdaStreamSafeWindow(~uint64_t(0)-100,0,~uint64_t(0)-96,~uint64_t(0)-4,4096,64,128));
    Device d;d.cursor=100;uint8_t data[128];std::memset(data,0x5a,sizeof(data));
    assert(HdaStreamPublish(d,0,256,data,128,4096,64,128)==HdaStreamPublishResult::Published);
    assert(d.device[255]==0 && d.device[256]==0x5a && d.device[383]==0x5a && d.device[384]==0);
    assert(d.writes==1 && d.flushes==1 && d.fences==1 && d.samples==2);
    // Wrapped samples and silence both reach the device across the boundary.
    Device w;w.cursor=3800;assert(HdaStreamPublish(w,0,4032,data,128,4096,64,128)==HdaStreamPublishResult::Published);
    assert(w.device[4095]==0x5a && w.device[0]==0x5a && w.writes==2 && w.flushes==2 && w.fences==1);
    assert(HdaStreamPublish(w,0,4032,nullptr,128,4096,64,128)==HdaStreamPublishResult::Published);
    assert(!w.device[4095] && !w.device[0]);
    Device slow;slow.cursor=100;slow.advance=80;
    assert(HdaStreamPublish(slow,0,256,data,128,4096,64,128)==HdaStreamPublishResult::Uncertain);
    assert(slow.device[256]==0x5a && slow.fences==1); // visible writes cannot be rolled back
    Device full;full.cursor=100;
    assert(HdaStreamPublish(full,0,128,data,128,4096,64,128)==HdaStreamPublishResult::Unsafe && !full.writes);
    const auto sampled=full.samples;
    assert(HdaStreamPublish(full,0,256,data,3,4096,64,128)==HdaStreamPublishResult::Invalid && full.samples==sampled);
    Device bad;bad.cursor=100;bad.failFlushAt=1;
    assert(HdaStreamPublish(bad,0,256,data,128,4096,64,128)==HdaStreamPublishResult::Uncertain && !bad.fences);
    Device sampledBad;sampledBad.cursor=100;sampledBad.failSampleAt=2;
    assert(HdaStreamPublish(sampledBad,0,256,data,128,4096,64,128)==HdaStreamPublishResult::Uncertain && sampledBad.fences==1);
    Device backwards;backwards.cursor=100;backwards.backward=true;
    assert(HdaStreamPublish(backwards,0,256,data,128,4096,64,128)==HdaStreamPublishResult::Uncertain);
    Device fenceBad;fenceBad.cursor=100;fenceBad.failFence=true;
    assert(HdaStreamPublish(fenceBad,0,256,data,128,4096,64,128)==HdaStreamPublishResult::Uncertain && fenceBad.samples==1);
    for(unsigned which=0;which<2;++which){Device split;split.cursor=3800;
        if(which)split.failFlushAt=2;else split.failWriteAt=2;
        assert(HdaStreamPublish(split,0,4032,data,128,4096,64,128)==HdaStreamPublishResult::Uncertain);
        assert(split.device[4095]==0x5a && !split.fences);}
    Device firstSample;firstSample.failSampleAt=1;
    assert(HdaStreamPublish(firstSample,0,256,data,128,4096,64,128)==HdaStreamPublishResult::Uncertain && !firstSample.writes);
    Device epoch;epoch.cursor=8292;
    assert(HdaStreamPublish(epoch,8192,8448,data,128,4096,64,128)==HdaStreamPublishResult::Published && epoch.device[256]==0x5a);
    assert(HdaStreamSafeWindow(100,0,256,400,4096,32,128));
    assert(HdaStreamSafeWindow(100,0,256,400,4096,128,128));
}

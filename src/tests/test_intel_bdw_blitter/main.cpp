#include <cassert>
#include <cstdint>
#include "intel_bdw_blitter.h"
using namespace brook;
int main() {
    BdwBlitCopy copy{{0x100000,448,128},{0x110000,448,128},16,4};
    uint32_t out[10]{};
    const uint32_t expected[10]={0x54f00008,0x03cc0080,0,0x00040010,0x00110000,0,0,0x80,0x00100000,0};
    assert(BdwEncodeLinearCopy(copy,out));
    for(unsigned i=0;i<10;++i)assert(out[i]==expected[i]);
    auto differing=copy;differing.destination.pitchBytes=256;differing.destination.allocationBytes=832;
    assert(BdwEncodeLinearCopy(differing,out));
    assert(out[1]==0x03cc0100 && out[7]==128);
    auto rejected=[&](const BdwBlitCopy& bad) {
        uint32_t packet[10];for(auto& v:packet)v=0xdeadbeef;
        assert(!BdwEncodeLinearCopy(bad,packet));
        for(auto v:packet)assert(v==0xdeadbeef);
    };
    auto bad=copy;bad.source.allocationBytes=447;rejected(bad);
    bad=copy;bad.destination.allocationBytes=447;rejected(bad);
    bad=copy;bad.destination.gpuAddress=0x100040;rejected(bad);
    bad=copy;bad.source.gpuAddress=0xffffffc0;bad.source.allocationBytes=448;rejected(bad);
    bad=copy;bad.source.gpuAddress++;rejected(bad);
    bad=copy;bad.source.pitchBytes=60;rejected(bad);
    bad=copy;bad.destination.pitchBytes=32768;rejected(bad);
    bad=copy;bad.widthPixels=0;rejected(bad);
    bad=copy;bad.heightPixels=32768;rejected(bad);
    bad=copy;bad.destination.gpuAddress=0x1001c0;assert(BdwEncodeLinearCopy(bad,out));
    BdwBlitRegion region{{0x200000,4096,128},{0x210000,8192,256},3,4,5,6,12,7};
    assert(BdwEncodeLinearRegion(region,out));
    assert(out[1]==0x03cc0100 && out[2]==0x00060005 && out[3]==0x000d0011 &&
           out[4]==0x210000 && out[6]==0x00040003 && out[7]==128 && out[8]==0x200000);
    auto rejectRegion=[&](const BdwBlitRegion& input){uint32_t packet[10];for(auto& x:packet)x=0xdeadbeef;
        assert(!BdwEncodeLinearRegion(input,packet));for(auto x:packet)assert(x==0xdeadbeef);};
    auto invalid=region;invalid.source.allocationBytes=10*128+15*4-1;rejectRegion(invalid);
    invalid=region;invalid.destinationX=60;rejectRegion(invalid);
    invalid=region;invalid.sourceY=32760;rejectRegion(invalid);
    invalid=region;invalid.destinationX=UINT32_MAX;rejectRegion(invalid);
    invalid=region;invalid.destination.gpuAddress=region.source.gpuAddress;rejectRegion(invalid);
}

#include <cassert>
#include <cstdint>
#include <vector>
#include <cmath>
#include "../../kernel/src/chrome_material.h"
#include "../../kernel/src/desktop_telemetry.h"
#include "../../shared/boot_splash.h"
int main(){
    // Reference circle rasterization at every supported native UI scale.
    for(unsigned radius:{5u,8u,10u})for(unsigned row=0;row<radius;++row){
        const unsigned inset=brook::ChromeCornerInset(radius,row);
        for(unsigned x=0;x<radius;++x){
            const double dx=radius-(x+0.5),dy=radius-(row+0.5);
            assert((x>=inset)==(dx*dx+dy*dy<=radius*radius));
        }
    }
    // Shadows have a transparent client hole and clean tile edges, with no
    // hidden opaque centre that could darken translucent chrome repeatedly.
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
        const auto alpha=brook::ChromeShadowPixel(x,y)>>24;
        if(x>=21&&x<43&&y>=16&&y<48)assert(alpha==0);
        if(x==0||y==0||x==63||y==63)assert(alpha==0);
    }
    brook::DesktopIntervals<> h;uint64_t now=1;h.Record(now);
    for(unsigned i=1;i<=100;++i){now+=i;h.Record(now);}
    auto s=h.Read(now);assert(s.count==100 && s.p80==80 && s.p95==95 && s.max==100);
    assert(h.Read(now+10001).count==0);
    h.Reset();assert(h.Read(now).count==0 && h.last==0);
    brook::DesktopIntervals<4> small;small.Record(1);
    for(unsigned i=1;i<=7;++i)small.Record(i*10+1);
    assert(small.count==4 && small.Read(71).p95==10);
    // Splash clips every logo/bar/glyph store, including tiny fallback modes.
    const uint32_t logo[4]={0x112233,0xaabbcc,0x4488cc,0};
    for(unsigned w:{1u,16u,80u})for(unsigned height:{1u,12u,80u}) {
        unsigned stride=w+3;std::vector<uint32_t> fb(stride*height+2,0xdeadbeef);
        brook::DrawBootSplash(fb.data()+1,w,height,stride,logo,2,2,255);
        assert(fb.front()==0xdeadbeef && fb.back()==0xdeadbeef);
        for(unsigned y=0;y<height;++y)for(unsigned x=w;x<stride;++x)assert(fb[1+y*stride+x]==0xdeadbeef);
    }
    std::vector<uint32_t> fb(80*80);brook::DrawBootSplash(fb.data(),80,80,80,logo,2,2,0,true,true);
    assert(fb[26*80+39]==0x332211);
}

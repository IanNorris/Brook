#include <cassert>
#include <cstdint>
#include <vector>
#include <cmath>
#include "../../shared/brook_appearance.h"
#include "../../shared/brook_taskbar.h"
#include "../../kernel/src/desktop_pointer_route.h"
#include "../../kernel/src/chrome_material.h"
#include "../../kernel/src/desktop_telemetry.h"
#include "../../shared/boot_splash.h"
int main(){
    brook::DesktopTitleClicks clicks;
    assert(!clicks.Press(7,100,30,0,6));assert(clicks.Press(7,102,31,200,6));
    assert(!clicks.Press(7,100,30,250,6));assert(!clicks.Press(8,100,30,300,6));
    clicks.Reset();assert(!clicks.Press(8,100,30,400,6));
    clicks.Motion(140,30);assert(!clicks.Press(8,100,30,500,6));
    assert(!clicks.Press(8,100,30,1000,6));assert(clicks.Press(8,100,30,1300,6));
    assert(BROOK_TASKBAR_DEFAULT==(BROOK_TASKBAR_CPU|BROOK_TASKBAR_MEMORY));
    for(unsigned mask=0;mask<=BROOK_TASKBAR_ALL;++mask)assert(brook_taskbar_metrics_valid(mask));
    assert(!brook_taskbar_metrics_valid(64) && !brook_taskbar_metrics_valid(0xffffffffu));
    assert(brook_taskbar_metrics_setting(3,"taskbar_metrics=0")==0);
    assert(brook_taskbar_metrics_setting(3,"taskbar_metrics=63\r")==63);
    for(const char* invalid:{"taskbar_metrics=64","taskbar_metrics=-1","taskbar_metrics=1000","taskbar_metrics=","taskbar_metrics=3junk","theme=1"})
        assert(brook_taskbar_metrics_setting(BROOK_TASKBAR_DEFAULT,invalid)==BROOK_TASKBAR_DEFAULT);
    assert(brook_appearance_valid(BROOK_APPEARANCE_DEFAULT));
    for(unsigned dark:{0u,1u})for(unsigned perf:{0u,1u})for(unsigned opacity:{40u,80u,100u})for(unsigned blur=0;blur<4;++blur){
        auto value=brook_appearance_pack(dark,perf,opacity,blur);assert(brook_appearance_valid(value));
        assert(brook_appearance_opacity(value)==opacity && brook_appearance_blur(value)==blur);
        assert((value&BROOK_APPEARANCE_DARK)==dark);
        auto normal=value&~BROOK_APPEARANCE_PERFORMANCE;
        assert(((value|BROOK_APPEARANCE_PERFORMANCE)&~BROOK_APPEARANCE_PERFORMANCE)==normal);
    }
    for(unsigned invalid:{0u,39u<<8,101u<<8,0xffffffffu,BROOK_APPEARANCE_DEFAULT|4u})assert(!brook_appearance_valid(invalid));
    auto value=brook_appearance_setting(BROOK_APPEARANCE_DEFAULT,"dark=0");assert(!(value&1));
    value=brook_appearance_setting(value,"opacity=100");assert(brook_appearance_opacity(value)==100);
    for(const char* invalid:{"opacity=-1","opacity=300","opacity=40junk","opacity=1000","blur=4","dark=2"})
        assert(brook_appearance_setting(value,invalid)==value);
    value=brook_appearance_setting(value,"blur=0");assert(brook_appearance_blur_strength(value)==0);
    // Opaque foreground stays opaque, even over a 40%-opaque material fill.
    assert(brook::ChromePixelOver(0x6628466f,0xffeeeeee)==0xffeeeeee);
    auto aa=brook::ChromePixelOver(0x6628466f,0x80eeeeee);assert((aa>>24)>0x66 && (aa>>24)<255);
    assert((brook::ChromePixelOver(0x0028466f,0x80eeeeee)>>24)==255);
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
    brook::ChromeMaterialCache material;
    for(unsigned scale:{100u,150u,200u}){
        const unsigned radius=(7*scale+50)/100,height=36*scale/100;
        assert(material.Ensure(0x28466f,0x253851,204,height,radius,2*scale/100));
        const auto saved=material.rows[1][0];
        for(unsigned frame=0;frame<120;++frame)
            assert(!material.Ensure(0x28466f,0x253851,204,height,radius,2*scale/100));
        assert(saved==material.rows[1][0]);
        unsigned bands=1;
        for(unsigned row=0;row<radius;row+=(radius+4)/5)++bands;
        assert(bands<=6);
        bool partial=false;
        for(unsigned y=0;y<radius;++y)for(unsigned x=0;x<radius;++x){
            auto coverage=material.top.coverage[y][x];
            assert(coverage==material.top.coverage[x][y]);
            assert(material.top.inner[y][x]<=coverage);
            partial|=coverage>0&&coverage<255;
            const auto pixel=brook::ChromeMaskedRim(0xcc28466f,coverage,material.top.inner[y][x],0xe6ddeeff);
            if(!coverage)assert(pixel==0);
            assert((pixel>>24)<=coverage);
        }
        assert(partial);
    }
    assert(material.Ensure(0xd4e3f5,0xd0d9e4,102,72,14,4));
    assert((material.rows[1][0]>>24)==102);
    assert(brook::ChromeMaskedRim(0xcc28466f,255,255,0xffffffff)==0xcc28466f);
    assert(brook::ChromeMaskedRim(0xcc28466f,255,0,0xe6ddeeff)==0xe6ddeeff);
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

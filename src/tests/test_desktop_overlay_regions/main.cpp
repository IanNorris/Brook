#include "../../kernel/src/desktop_overlay_regions.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"OVERLAY_REGIONS line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

int main(){
    using namespace brook;
    uint32_t random=17;
    for(unsigned iteration=0;iteration<10000;++iteration){
        DesktopOverlayRect rects[4]{};
        for(auto& r:rects){
            auto next=[&](){random=random*1664525u+1013904223u;return random;};
            r={int32_t(next()%25)-8,int32_t(next()%22)-7,next()%15,next()%14};
        }
        DesktopOverlayBands bands{};CHECK(DesktopOverlayMakeBands(12,9,rects,4,bands));CHECK(bands.count<=7);
        for(unsigned y=0;y<9;++y)for(unsigned x=0;x<12;++x){
            bool covered=false;
            for(const auto& r:rects)if(int64_t(x)>=r.x && int64_t(y)>=r.y &&
                int64_t(x)<int64_t(r.x)+r.width && int64_t(y)<int64_t(r.y)+r.height)covered=true;
            unsigned output=0;
            for(uint32_t i=0;i<bands.count;++i){const auto& b=bands.bands[i];
                CHECK(b.width && b.height && b.x+b.width<=12 && b.y+b.height<=9);
                if(x>=b.x && x<b.x+b.width && y>=b.y && y<b.y+b.height)++output;
            }
            CHECK(output<=1 && (!covered || output==1));
        }
    }
    DesktopOverlayBands invalid{};CHECK(!DesktopOverlayMakeBands(0,9,nullptr,0,invalid));
    CHECK(!DesktopOverlayMakeBands(12,9,nullptr,1,invalid));
    CHECK(!DesktopOverlayMakeBands(12,9,nullptr,5,invalid));
    uint32_t image[12*9],packed[12*9],expected[12*9];
    for(unsigned i=0;i<12*9;++i)image[i]=i*17+3;
    for(auto band:{DesktopOverlayBand{0,2,12,5},DesktopOverlayBand{3,1,6,7}}){
        std::memset(packed,0,sizeof(packed));std::memset(expected,0,sizeof(expected));
        for(uint32_t y=0;y<band.height;++y)
            std::memcpy(expected+y*band.width,image+(band.y+y)*12+band.x,band.width*4);
        unsigned calls=0;
        auto copy=[&](uint64_t from,uint64_t to,uint32_t n){++calls;CHECK(n<=19);
            std::memcpy(reinterpret_cast<uint8_t*>(packed)+to,reinterpret_cast<uint8_t*>(image)+from,n);return true;};
        CHECK(DesktopOverlayPackedRead(band,12,9,19,copy) && calls);
        CHECK(!std::memcmp(packed,expected,band.width*band.height*4));
        unsigned failures=0;
        CHECK(!DesktopOverlayPackedRead(band,12,9,19,[&](uint64_t,uint64_t,uint32_t){return ++failures<2;}));
    }
    CHECK(!DesktopOverlayPackedRead(DesktopOverlayBand{11,0,2,1},12,9,19,[](uint64_t,uint64_t,uint32_t){return true;}));
    // Opacity: a band is opaque only where opaque rectangles cover all of it.
    {
        // Taskbar (opaque) and launcher (opaque) in separate rows.
        const DesktopOverlayRect shell[2]={{0,40,64,8},{2,10,20,28}};
        DesktopOverlayBands b{};CHECK(DesktopOverlayMakeBands(64,48,shell,2,b,3u));
        CHECK(b.count==2 && b.opaque==3u);
        // A translucent (pointer) rectangle sticking out of the launcher's rows
        // leaves those rows' band translucent, not the launcher-only rows.
        const DesktopOverlayRect withCursor[3]={{0,40,64,8},{2,10,20,28},{18,30,12,12}};
        CHECK(DesktopOverlayMakeBands(64,48,withCursor,3,b,3u));
        for(uint32_t i=0;i<b.count;++i){const auto& x=b.bands[i];const bool opaque=(b.opaque>>i)&1u;
            const bool inLauncher=x.y>=10 && x.y+x.height<=38 && x.x>=2 && x.x+x.width<=22;
            const bool inTaskbar=x.y>=40 && x.y+x.height<=48;
            CHECK(opaque==(inLauncher || inTaskbar));}
        CHECK(b.opaque!=0 && b.opaque!=(1u<<b.count)-1u);
        // Nothing marked opaque: no opaque bands.
        CHECK(DesktopOverlayMakeBands(64,48,shell,2,b,0u) && b.opaque==0);
        // Two opaque rectangles side by side with a gap: their shared band is not opaque.
        const DesktopOverlayRect gap[2]={{0,0,10,8},{20,0,10,8}};
        CHECK(DesktopOverlayMakeBands(64,48,gap,2,b,3u) && b.count==1 && b.opaque==0);
        const DesktopOverlayRect touch[2]={{0,0,10,8},{10,0,10,8}};
        CHECK(DesktopOverlayMakeBands(64,48,touch,2,b,3u) && b.count==1 && b.opaque==1u);
        // Equal rows of different opacity are never merged into one band.
        const DesktopOverlayRect stacked[2]={{0,0,10,8},{0,8,10,8}};
        CHECK(DesktopOverlayMakeBands(64,48,stacked,2,b,1u) && b.count==2 && b.opaque==1u);
        CHECK(DesktopOverlayMakeBands(64,48,stacked,2,b,3u) && b.count==1 && b.opaque==1u);
    }
    // Keys: nonzero, equal for equal pixels, different when a pixel changes.
    {
        static uint32_t img[64*48];for(unsigned i=0;i<64*48;++i)img[i]=0xff000000u|i;
        const DesktopOverlayRect shell[2]={{0,40,64,8},{2,10,20,28}};
        DesktopOverlayBands a{},b{};
        CHECK(DesktopOverlayMakeBands(64,48,shell,2,a,3u));DesktopOverlayKeyBands(img,64,48,a);
        CHECK(DesktopOverlayMakeBands(64,48,shell,2,b,3u));DesktopOverlayKeyBands(img,64,48,b);
        CHECK(a.keys[0] && a.keys[1] && a.keys[0]==b.keys[0] && a.keys[1]==b.keys[1] && a.keys[0]!=a.keys[1]);
        img[45*64+30]^=1;                     // taskbar pixel changes
        DesktopOverlayKeyBands(img,64,48,b);
        const unsigned taskbar=a.bands[0].y==40?0:1,launcher=1-taskbar;
        CHECK(b.keys[taskbar]!=a.keys[taskbar] && b.keys[launcher]==a.keys[launcher]);
    }
    std::puts("OVERLAY_REGIONS PASS");
}

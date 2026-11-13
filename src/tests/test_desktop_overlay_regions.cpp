#include "../kernel/src/desktop_overlay_regions.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

int main(){
    using namespace brook;
    uint32_t random=17;
    for(unsigned iteration=0;iteration<10000;++iteration){
        DesktopOverlayRect rects[4]{};
        for(auto& r:rects){
            auto next=[&](){random=random*1664525u+1013904223u;return random;};
            r={int32_t(next()%25)-8,int32_t(next()%22)-7,next()%15,next()%14};
        }
        DesktopOverlayBands bands{};assert(DesktopOverlayMakeBands(12,9,rects,4,bands));assert(bands.count<=7);
        for(unsigned y=0;y<9;++y)for(unsigned x=0;x<12;++x){
            bool covered=false;
            for(const auto& r:rects)if(int64_t(x)>=r.x && int64_t(y)>=r.y &&
                int64_t(x)<int64_t(r.x)+r.width && int64_t(y)<int64_t(r.y)+r.height)covered=true;
            unsigned output=0;
            for(uint32_t i=0;i<bands.count;++i){const auto& b=bands.bands[i];
                assert(b.width && b.height && b.x+b.width<=12 && b.y+b.height<=9);
                if(x>=b.x && x<b.x+b.width && y>=b.y && y<b.y+b.height)++output;
            }
            assert(output<=1 && (!covered || output==1));
        }
    }
    DesktopOverlayBands invalid{};assert(!DesktopOverlayMakeBands(0,9,nullptr,0,invalid));
    assert(!DesktopOverlayMakeBands(12,9,nullptr,1,invalid));
    assert(!DesktopOverlayMakeBands(12,9,nullptr,5,invalid));
    uint32_t image[12*9],packed[12*9],expected[12*9];
    for(unsigned i=0;i<12*9;++i)image[i]=i*17+3;
    for(auto band:{DesktopOverlayBand{0,2,12,5},DesktopOverlayBand{3,1,6,7}}){
        std::memset(packed,0,sizeof(packed));std::memset(expected,0,sizeof(expected));
        for(uint32_t y=0;y<band.height;++y)
            std::memcpy(expected+y*band.width,image+(band.y+y)*12+band.x,band.width*4);
        unsigned calls=0;
        auto copy=[&](uint64_t from,uint64_t to,uint32_t n){++calls;assert(n<=19);
            std::memcpy(reinterpret_cast<uint8_t*>(packed)+to,reinterpret_cast<uint8_t*>(image)+from,n);return true;};
        assert(DesktopOverlayPackedRead(band,12,9,19,copy) && calls);
        assert(!std::memcmp(packed,expected,band.width*band.height*4));
        unsigned failures=0;
        assert(!DesktopOverlayPackedRead(band,12,9,19,[&](uint64_t,uint64_t,uint32_t){return ++failures<2;}));
    }
    assert(!DesktopOverlayPackedRead(DesktopOverlayBand{11,0,2,1},12,9,19,[](uint64_t,uint64_t,uint32_t){return true;}));
}

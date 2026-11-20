#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "intel_bdw_scanout_capture.h"

using namespace brook;

namespace {
constexpr uint32_t Enable=1u<<31,Tiled=1u<<10,Xrgb=6u<<26,Xbgr=14u<<26;
uint32_t Pixel(uint32_t x,uint32_t y){return 0x01000000u*((x+y)&0x7f)|(x<<8)|(y&0xff)|((x*7+y)&0xff)<<16;}

// Build a surface in separately allocated pages (out of order in memory) so
// the copy must use the page table, not assume contiguity.
struct Surface {
    std::vector<std::vector<uint8_t>> pages;
    const uint8_t* Page(uint32_t i) const {return i<pages.size()?pages[i].data():nullptr;}
};
Surface Linear(uint32_t width,uint32_t height,uint32_t stride){
    const uint32_t count=uint32_t((uint64_t(height-1)*stride+width*4+4095)/4096);
    Surface s;s.pages.assign(count,std::vector<uint8_t>(4096,0xee));
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
        const uint64_t at=uint64_t(y)*stride+x*4;const uint32_t p=Pixel(x,y);
        memcpy(&s.pages[at>>12][at&4095],&p,4);
    }
    return s;
}
Surface XTiled(uint32_t width,uint32_t height,uint32_t stride){
    const uint32_t tilesPerRow=stride/512,count=((height+7)/8)*tilesPerRow;
    Surface s;s.pages.assign(count,std::vector<uint8_t>(4096,0xee));
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
        const uint32_t tile=(y/8)*tilesPerRow+(x*4)/512,within=(y%8)*512+(x*4)%512,p=Pixel(x,y);
        memcpy(&s.pages[tile][within],&p,4);
    }
    return s;
}
void Check(const std::vector<uint32_t>& out,uint32_t width,uint32_t height,bool swap){
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
        uint32_t p=Pixel(x,y);if(swap)p=(p&0xff00ff00u)|((p>>16)&0xffu)|((p&0xffu)<<16);
        assert(out[uint64_t(y)*width+x]==p);
    }
}
}

int main(){
    BdwCaptureLayout layout{};
    // The Dell GOP mode: linear XRGB8888, stride 12800, 5625 pages.
    assert(BdwCaptureLayoutFrom(Enable|Xrgb,12800,0,3200,1800,layout));
    assert(!layout.tiled && !layout.swapRb && layout.pages==5625);
    // Refusals: plane off, unknown format, panning offset, short or unaligned stride.
    assert(!BdwCaptureLayoutFrom(Xrgb,12800,0,3200,1800,layout));
    assert(!BdwCaptureLayoutFrom(Enable|(10u<<26),12800,0,3200,1800,layout));
    assert(!BdwCaptureLayoutFrom(Enable|Xrgb,12800,64,3200,1800,layout));
    assert(!BdwCaptureLayoutFrom(Enable|Xrgb,12796,0,3200,1800,layout));
    assert(!BdwCaptureLayoutFrom(Enable|Xrgb|Tiled,12800+64,0,3200,1800,layout));
    // X-tiled client stride (multiple of 512): 225 tile rows x 26 tiles.
    assert(BdwCaptureLayoutFrom(Enable|Xrgb|Tiled,13312,0,3200,1800,layout) && layout.tiled && layout.pages==225*26);

    {   // Linear with a padded stride, rows straddling pages.
        const uint32_t w=200,h=37,stride=1024+64;
        auto surface=Linear(w,h,stride);
        assert(BdwCaptureLayoutFrom(Enable|Xrgb,stride,0,w,h,layout) && layout.pages==surface.pages.size());
        std::vector<uint32_t> out(w*h,0);
        assert(BdwCaptureCopy(out.data(),layout,[&](uint32_t i){return surface.Page(i);}));
        Check(out,w,h,false);
    }
    {   // X-tiled, width not a multiple of the 128-pixel tile, height not of 8.
        const uint32_t w=300,h=21,stride=1536;
        auto surface=XTiled(w,h,stride);
        assert(BdwCaptureLayoutFrom(Enable|Xrgb|Tiled,stride,0,w,h,layout) && layout.pages==surface.pages.size());
        std::vector<uint32_t> out(w*h,0);
        assert(BdwCaptureCopy(out.data(),layout,[&](uint32_t i){return surface.Page(i);}));
        Check(out,w,h,false);
    }
    {   // XBGR8888 is returned as B,G,R,X.
        const uint32_t w=64,h=4,stride=256;
        auto surface=Linear(w,h,stride);
        assert(BdwCaptureLayoutFrom(Enable|Xbgr,stride,0,w,h,layout) && layout.swapRb);
        std::vector<uint32_t> out(w*h,0);
        assert(BdwCaptureCopy(out.data(),layout,[&](uint32_t i){return surface.Page(i);}));
        Check(out,w,h,true);
    }
    {   // A missing page fails the copy.
        const uint32_t w=1024,h=8,stride=4096;
        auto surface=Linear(w,h,stride);
        assert(BdwCaptureLayoutFrom(Enable|Xrgb,stride,0,w,h,layout));
        std::vector<uint32_t> out(w*h,0);
        assert(!BdwCaptureCopy(out.data(),layout,[&](uint32_t i){return i==3?nullptr:surface.Page(i);}));
    }
    {   // Cursor position is sign-magnitude per axis.
        int32_t x=0,y=0;
        BdwCaptureCursorPosition((20u<<16)|10u,x,y);assert(x==10 && y==20);
        BdwCaptureCursorPosition(0x80000000u|(5u<<16)|0x8000u|7u,x,y);assert(x==-7 && y==-5);
    }
    {   // Premultiplied cursor over a frame, clipped at the edges.
        std::vector<uint32_t> frame(100*50,0xff204060u),cursor(64*64,0);
        cursor[0]=0xff112233u;          // opaque
        cursor[1]=0x80400000u;          // half alpha, premultiplied red 0x40
        cursor[63*64+63]=0xffffffffu;   // lands off screen
        BdwCaptureCursorComposite(frame.data(),100,50,cursor.data(),-1+1,0);
        assert(frame[0]==0xff112233u);
        const uint32_t d=0xff204060u,a=0x80;
        auto mix=[&](unsigned shift,uint32_t s){return (s+(((d>>shift)&255u)*(255u-a)+127)/255)<<shift;};
        assert(frame[1]==(0xff000000u|mix(16,0x40)|mix(8,0)|mix(0,0)));
        assert(frame[2]==0xff204060u); // transparent pixels untouched
        std::vector<uint32_t> edge(10*10,0);
        BdwCaptureCursorComposite(edge.data(),10,10,cursor.data(),-60,-60); // only row/col 60..63 visible
        assert(edge[3*10+3]==0xffffffffu && edge[0]==0);
    }
    return 0;
}

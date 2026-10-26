#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include "gpu_quad_math.h"
using namespace brook;
static double decoded(uint32_t bits) {float f;std::memcpy(&f,&bits,4);return f;}
static void fractions() {
    for(int32_t dimension:{1,3,17,1920,8192,16384}) {
        for(int32_t point:{INT32_MIN,-16383,-100,-1,0,1,7,1920,16384,INT32_MAX}) {
            const double want=2.0*point/dimension-1.0;
            const double got=decoded(GpuNdcBits(point,dimension));
            assert(std::isfinite(got));
            assert(std::abs(got-want)<=std::abs(want)*2e-7+1.3e-7);
        }
    }
    assert(decoded(GpuNdcBits(-8191,1))==-16383.0);
    assert(decoded(GpuNdcBits(8192,1))==16383.0);
    assert(GpuFractionBits(INT64_MAX,1)==0);
    assert(GpuFractionBits(INT64_MIN,1)==0);
    assert(GpuFractionBits(1,0)==0 && GpuFractionBits(1,-1)==0);
    assert(decoded(GpuFractionBits(1,2))==0.5);
    assert(decoded(GpuFractionBits(-3,2))==-1.5);
}
// Rasterize the production destination/NDC math over an asymmetric coordinate
// image. The reference maps each visible pixel against the original rectangle,
// so shrinking a partly clipped quad while retaining full UVs fails this test.
static void raster(int32_t x,int32_t y,uint32_t width,uint32_t height,
                   uint32_t textureW,uint32_t textureH) {
    constexpr uint32_t screenW=23,screenH=19;
    GpuQuadBounds b{};
    const bool visible=GpuWindowQuad(x,y,width,height,screenW,screenH,b);
    const bool expected=width && height && int64_t(x)<screenW && int64_t(y)<screenH &&
                        int64_t(x)+width>0 && int64_t(y)+height>0;
    assert(visible==expected);
    if(!visible)return;
    const double left=(decoded(GpuNdcBits(b.left,screenW))+1)*screenW/2;
    const double right=(decoded(GpuNdcBits(b.right,screenW))+1)*screenW/2;
    const double top=(decoded(GpuNdcBits(b.top,screenH))+1)*screenH/2;
    const double bottom=(decoded(GpuNdcBits(b.bottom,screenH))+1)*screenH/2;
    for(unsigned py=0;py<screenH;++py)for(unsigned px=0;px<screenW;++px) {
        const double cx=px+0.5,cy=py+0.5;
        const bool covered=cx>=x && cy>=y && cx<int64_t(x)+width && cy<int64_t(y)+height;
        assert(covered==(cx>=left && cy>=top && cx<right && cy<bottom));
        if(!covered)continue;
        const double u=(cx-left)/(right-left),v=(cy-top)/(bottom-top);
        const double expectedU=(cx-x)/width,expectedV=(cy-y)/height;
        // Unequal/odd texture and window sizes also retain their full mapping.
        assert(std::abs((u-expectedU)*textureW)<0.0001);
        assert(std::abs((v-expectedV)*textureH)<0.0001);
    }
}
int main() {
    fractions();
    for(int32_t x:{-30,-10,-1,0,3,18,22,23,30})
        for(int32_t y:{-25,-7,-1,0,4,15,18,19,25})
            for(uint32_t width:{7u,11u,26u})for(uint32_t height:{5u,9u,21u}) {
                raster(x,y,width,height,7,5);
                raster(x,y,width,height,11,3);
            }
    GpuQuadBounds b{};
    assert(GpuWindowQuad(100,100,1920,1036,1920,1080,b));
    assert(b.left==100 && b.top==100 && b.right==2020 && b.bottom==1136);
    assert(!GpuQuadDestination(INT32_MAX,0,1,1,b));
    assert(!GpuQuadDestination(0,INT32_MAX,1,1,b));
    assert(!GpuQuadDestination(0,0,0,1,b));
    assert(!GpuWindowQuad(0,0,1,1,0,1,b));
    assert(GpuQuadDestination(INT32_MIN,0,UINT32_MAX,1,b));
    assert(b.right==INT32_MAX);
    std::puts("GPU_WINDOW_QUAD_PASS signed bounds, full mapping, four edges, scaled raster, wide NDC");
}

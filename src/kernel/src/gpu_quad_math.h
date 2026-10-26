#pragma once
#include <stdint.h>

namespace brook {
struct GpuQuadBounds { int32_t left,top,right,bottom; };

inline bool GpuQuadDestination(int32_t x,int32_t y,uint32_t w,uint32_t h,GpuQuadBounds& out)
{
    out={};
    const int64_t right=int64_t(x)+w,bottom=int64_t(y)+h;
    constexpr int64_t maxEndpoint=(int64_t{1}<<31)-1;
    if(!w || !h || right>maxEndpoint || bottom>maxEndpoint)return false;
    out={x,y,int32_t(right),int32_t(bottom)};return true;
}

// Preserve the original quad. Shrinking it to its visible extent while keeping
// full texture UVs rescales the image and separates displayed/input coordinates.
inline bool GpuWindowQuad(int32_t x,int32_t y,uint32_t w,uint32_t h,
                          uint32_t screenW,uint32_t screenH,GpuQuadBounds& out)
{
    if(!GpuQuadDestination(x,y,w,h,out))return false;
    if(!screenW || !screenH || int64_t(x)>=screenW || int64_t(y)>=screenH ||
       out.right<=0 || out.bottom<=0) {out={};return false;}
    return true;
}

// Q23 conversion for the bounded UV/NDC numerators used by the compositor.
// Unlike an int32_t quotient, the 64-bit normalization also represents large
// offscreen NDC vertices (e.g. a wide window crossing a one-pixel viewport).
// All validated scene UV numerators and int32_t pixel coordinates fit this
// multiplication bound. Invalid inputs are rejected without signed overflow.
inline uint32_t GpuFractionBits(int64_t num,int64_t den)
{
    constexpr int64_t scale=int64_t{1}<<23;
    constexpr int64_t limit=(int64_t{1}<<40)-1;
    if(!num || den<=0 || num>limit || num< -limit)return 0;
    const int64_t q=(num*scale)/den;
    if(!q)return 0;
    const uint32_t sign=q<0?0x80000000u:0;
    uint64_t mantissa=q<0?uint64_t(-(q+1))+1:uint64_t(q);
    int shift=0;
    while(mantissa>=(uint64_t{1}<<24)) {mantissa>>=1;++shift;}
    while(mantissa<(uint64_t{1}<<23)) {mantissa<<=1;--shift;}
    return sign | (uint32_t(127+shift)<<23) | (uint32_t(mantissa)&0x7fffffu);
}

inline uint32_t GpuNdcBits(int32_t px,int32_t dimension)
{
    return GpuFractionBits(int64_t{2}*px-dimension,dimension);
}
}

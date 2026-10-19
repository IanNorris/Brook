#pragma once
#include <stdint.h>
#include "gpu_scene_abi.h"

namespace brook {
inline constexpr uint32_t GPU_SCENE_MAX_LAYERS=64;
inline constexpr uint32_t GPU_SCENE_MAX_DIMENSION=16384;
// Exact half-integer viewport scale/translation for supported dimensions,
// including odd sizes. Normalized UV fixed-point helpers cannot encode large
// viewport values without overflowing their intermediate signed 32-bit value.
inline uint32_t GpuViewportHalfBits(uint32_t dimension) {
    if (!dimension) return 0;
    uint32_t top=0;
    for (uint32_t value=dimension;value>1;value>>=1) ++top;
    const uint32_t mantissa=top<=23?dimension<<(23-top):dimension>>(top-23);
    return ((top+126)<<23) | (mantissa&0x7fffffu);
}
// A flattened, ordered draw list. Coordinates and clip are root-relative;
// source/destination rectangles may scale independently. All colors are
// premultiplied BGRA, including opacity applied to both RGB and alpha.
struct GpuSceneLayer {
    uint32_t texture,sx,sy,sw,sh;
    int32_t x,y;
    uint32_t w,h,opacity;
    int32_t clipX,clipY;
    uint32_t clipW,clipH;
    uint32_t flags=0;
};
struct GpuSceneRect {
    bool visible;
    int32_t x,y,right,bottom;
    int64_t u0,u1,uDen,v0,v1,vDen;
};
// Integer rational UVs preserve fractional source positions after scaled
// clipping. Wide intermediate arithmetic handles arbitrary signed offsets.
inline bool GpuSceneClip(const GpuSceneLayer& l,uint32_t sourceW,uint32_t sourceH,
                         uint32_t rootW,uint32_t rootH,GpuSceneRect& out) {
    out={};
    constexpr uint32_t max=GPU_SCENE_MAX_DIMENSION;
    if (l.flags&~SCENE_LAYER_FLAGS_MASK) return false;
    const uint32_t transform=(l.flags>>SCENE_LAYER_TRANSFORM_SHIFT)&7;
    const uint32_t units=(l.flags&SCENE_LAYER_FIXED8)?256:1;
    const uint32_t tw=(transform&1)?sourceH:sourceW,th=(transform&1)?sourceW:sourceH;
    const uint64_t width=uint64_t(tw)*units,height=uint64_t(th)*units;
    if (!sourceW || !sourceH || !rootW || !rootH || sourceW>max || sourceH>max ||
        rootW>max || rootH>max || !l.sw || !l.sh || !l.w || !l.h ||
        l.w>max || l.h>max || l.opacity>255 || l.clipW>max || l.clipH>max ||
        l.sx>width || l.sy>height || l.sw>width-l.sx || l.sh>height-l.sy) return false;
    auto min=[](int64_t a,int64_t b) { return a<b?a:b; };
    auto maxOf=[](int64_t a,int64_t b) { return a>b?a:b; };
    const int64_t left=maxOf(0,maxOf(l.x,l.clipX));
    const int64_t top=maxOf(0,maxOf(l.y,l.clipY));
    const int64_t right=min(rootW,min(int64_t(l.x)+l.w,int64_t(l.clipX)+l.clipW));
    const int64_t bottom=min(rootH,min(int64_t(l.y)+l.h,int64_t(l.clipY)+l.clipH));
    if (left>=right || top>=bottom || !l.opacity) return true;
    out={true,int32_t(left),int32_t(top),int32_t(right),int32_t(bottom),
        int64_t(l.sx)*l.w+(left-l.x)*l.sw,
        int64_t(l.sx)*l.w+(right-l.x)*l.sw,int64_t(width)*l.w,
        int64_t(l.sy)*l.h+(top-l.y)*l.sh,
        int64_t(l.sy)*l.h+(bottom-l.y)*l.sh,int64_t(height)*l.h};
    return true;
}
// Convert a clipped logical corner to physical texture UVs. Each axis keeps
// its own exact denominator through rotations and reflections.
struct GpuSceneUv { int64_t u,uDen,v,vDen; };
inline GpuSceneUv GpuSceneCorner(const GpuSceneRect& r,uint32_t flags,bool right,bool bottom) {
    int64_t u=right?r.u1:r.u0,v=bottom?r.v1:r.v0,ud=r.uDen,vd=r.vDen;
    const uint32_t t=(flags>>SCENE_LAYER_TRANSFORM_SHIFT)&7;
    if (t&4) u=ud-u;
    switch (t&3) {
    case 1:return {v,vd,ud-u,ud};
    case 2:return {ud-u,ud,vd-v,vd};
    case 3:return {vd-v,vd,u,ud};
    default:return {u,ud,v,vd};
    }
}
enum class GpuSceneStep { UploadVertices,RenderAndFence,RestoreAndFence };
template<class Step> bool GpuSceneSequence(Step step) {
    return step(GpuSceneStep::UploadVertices) && step(GpuSceneStep::RenderAndFence) &&
           step(GpuSceneStep::RestoreAndFence);
}
}

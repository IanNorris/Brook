#pragma once
#include "window.h"
#include "gpu_scene.h"
#include "../../shared/desktop_layers_abi.h"

namespace brook {
// The exported legacy source is opaque. Invert a snapshot of its terminal
// caret in the copied bytes, never in the producer's framebuffer.
inline void DesktopInvertCaret(const WmDesktopLayer& layer,uint64_t offset,void* pixels,uint64_t bytes){
    if(!pixels || !layer.caretWidth || !layer.caretHeight || !layer.stride || (offset&3) || (bytes&3))return;
    const uint64_t first=offset/4,end=first+bytes/4;
    if(end<first || first==end)return;
    uint64_t top=layer.caretY,bottom=top+layer.caretHeight;
    uint64_t left=layer.caretX,right=left+layer.caretWidth;
    if(bottom>layer.height)bottom=layer.height;if(right>layer.width)right=layer.width;
    if(left>=right || top>=bottom)return;
    const uint64_t firstRow=first/layer.stride,lastRow=(end-1)/layer.stride;
    if(top<firstRow)top=firstRow;if(bottom>lastRow+1)bottom=lastRow+1;
    auto* words=static_cast<uint32_t*>(pixels);
    for(uint64_t y=top;y<bottom;++y){
        uint64_t begin=y*layer.stride+left,stop=y*layer.stride+right;
        if(begin<first)begin=first;if(stop>end)stop=end;
        for(uint64_t i=begin;i<stop;++i)words[i-first]^=0x00ffffffu;
    }
}
struct DesktopCaptureCounters { uint64_t calls,keeps,keepMisses,captures,bytesCopied,bytesKept,occluded,bytesOccluded; };
// Client-layer capture totals for `desktop capture status` (syscall.cpp).
extern DesktopCaptureCounters g_desktopCaptureStats;
// The caller holds a copy of exactly this publication: same window identity,
// published content generation and geometry. Terminal carets are inverted
// into the copy on a timer, independent of publications, so never kept.
inline bool DesktopLayerKeepable(const WmDesktopLayer& l,const BrookDesktopLayer& held){
    if(!l.height || uint64_t(l.stride)>~uint64_t{0}/4/l.height)return false;
    const uint64_t bytes=uint64_t(l.stride)*l.height*4;
    return held.window==l.window && held.incarnation==l.incarnation && l.generation!=~uint64_t{0} &&
        held.generation==l.generation && held.bytes==bytes && held.width==l.width && held.height==l.height &&
        held.stride==l.stride && !l.caretWidth && !l.caretHeight;
}
// Export bounds. There is no fixed total: the real limits are the WM
// backing (l.bytes) and the destination the renderer allocated
// (o.capacity), both checked per layer. What remains guards against corrupt
// sizes only:
//  - a layer is at most GPU_SCENE_MAX_DIMENSION in stride and height (no
//    scene can sample more), so at most 1 GiB;
//  - the bytes one capture actually copies (not kept, skipped or occluded
//    layers) are at most BROOK_DESKTOP_LAYER_LIMIT whole outputs: the
//    renderer composites at most that many layers and each is clipped to the
//    output, so only a corrupt snapshot comes near it (3200x1800: 703 MiB).
//    Exceeding it is E2BIG, and the caller keeps or skips layers and retries.
// Query copies nothing and has no total. (Was a 64 MiB total over every
// layer, kept or not: six large Dell windows, 69.2 MB, failed every frame.)
inline constexpr uint64_t DesktopLayerMaxBytes=uint64_t(GPU_SCENE_MAX_DIMENSION)*GPU_SCENE_MAX_DIMENSION*4;
inline constexpr uint64_t DesktopLayerFallbackOutputBytes=4096ull*2160*4; // the renderer's largest output
inline uint64_t DesktopLayerCopyLimit(uint32_t outputWidth,uint32_t outputHeight){
    const bool known=outputWidth && outputHeight && outputWidth<=GPU_SCENE_MAX_DIMENSION && outputHeight<=GPU_SCENE_MAX_DIMENSION;
    return (known?uint64_t(outputWidth)*outputHeight*4:DesktopLayerFallbackOutputBytes)*BROOK_DESKTOP_LAYER_LIMIT;
}
// The output rectangle a layer's content is drawn to, exactly as the
// renderer (desktop_content_layer.h) and the software path draw it: native
// buffers 1:1 cropped to the client size, legacy buffers integer-upscaled
// and clipped to the client rectangle.
struct DesktopLayerRect { int64_t left,top,right,bottom; };
inline DesktopLayerRect DesktopLayerContentRect(const WmDesktopLayer& l){
    const uint64_t scale=l.legacyOpaque?l.upscale:1;
    uint64_t w=uint64_t(l.width)*scale,h=uint64_t(l.height)*scale;
    if(w>l.clientWidth)w=l.clientWidth;if(h>l.clientHeight)h=l.clientHeight;
    return {l.clientX,l.clientY,int64_t(l.clientX)+int64_t(w),int64_t(l.clientY)+int64_t(h)};
}
// Layer index is covered by one layer above it that is certainly opaque
// where it draws: only a legacy framebuffer (always drawn opaque, never a
// GPU offer) at full opacity without blur. Native buffers carry straight
// alpha whose content the kernel cannot see, so they never occlude.
inline bool DesktopLayerOccluded(const WmDesktopLayers& source,uint32_t index){
    const auto& l=source.layers[index];
    if(!l.width || !l.height || !l.clientWidth || !l.clientHeight || !l.upscale)return false;
    const DesktopLayerRect r=DesktopLayerContentRect(l);
    if(r.left>=r.right || r.top>=r.bottom)return false;
    for(uint32_t j=index+1;j<source.count;++j){
        const auto& a=source.layers[j];
        if(!a.legacyOpaque || a.opacity!=255 || a.blurRadius || !a.upscale)continue;
        const DesktopLayerRect c=DesktopLayerContentRect(a);
        if(c.left<=r.left && c.top<=r.top && c.right>=r.right && c.bottom>=r.bottom)return true;
    }
    return false;
}
struct DesktopLayerExportOptions {
    uint64_t copyLimit=DesktopLayerCopyLimit(0,0);
    // BROOK_DESKTOP_LAYERS_CULL_OCCLUDED: do not copy occluded layers and
    // flag them BROOK_DESKTOP_LAYER_OCCLUDED (capture only).
    bool cullOccluded=false;
};
// Snapshot is retained by the caller. Sink reads a bounded range of pinned pages and
// copies to a user destination; current rechecks the actual window state.
// A failed capture must never publish its metadata as a completed frame.
template<class Sink,class Current>
int64_t ExportDesktopLayers(const WmDesktopLayers& source,BrookDesktopLayers& wire,
                            bool capture,Sink sink,Current current,uint64_t chunkLimit=4096,uint32_t skipMask=0,
                            uint32_t keepMask=0,DesktopCaptureCounters* counters=nullptr,
                            DesktopLayerExportOptions options=DesktopLayerExportOptions{}) {
    if(!chunkLimit || chunkLimit>65536)return -22;
    if(wire.version!=BROOK_DESKTOP_LAYERS_VERSION || wire.reserved || source.count>BROOK_DESKTOP_LAYER_LIMIT)return -22;
    if(capture && wire.count!=source.count)return -11;
    if(skipMask && (!capture || (source.count<32 && (skipMask>>source.count))))return -22;
    if(keepMask && (!capture || (keepMask&skipMask) || (source.count<32 && (keepMask>>source.count))))return -22;
    if(options.cullOccluded && !capture)return -22;
    if(skipMask|keepMask)for(uint32_t i=0;i<source.count;++i)
        if((wire.layers[i].window!=source.layers[i].window || wire.layers[i].incarnation!=source.layers[i].incarnation))return -11;
    // A keep layer's input metadata describes the pixels the caller holds.
    // Decide before the metadata below is overwritten with current values.
    uint32_t kept=0,occluded=0;DesktopCaptureCounters tally{};
    for(uint32_t i=0;options.cullOccluded && i<source.count;++i)
        if(!(skipMask&(uint32_t{1}<<i)) && DesktopLayerOccluded(source,i))occluded|=uint32_t{1}<<i;
    for(uint32_t i=0;keepMask && i<source.count;++i){
        if(!(keepMask&(uint32_t{1}<<i)) || (occluded&(uint32_t{1}<<i)))continue;
        if(DesktopLayerKeepable(source.layers[i],wire.layers[i]))kept|=uint32_t{1}<<i;
    }
    uint64_t copying=0;
    for(uint32_t i=0;i<source.count;++i){
        const auto& l=source.layers[i];auto& o=wire.layers[i];const uint32_t bit=uint32_t{1}<<i;
        // This renderer cannot reproduce backdrop blur; leave the OS-owned
        // software path in charge instead of presenting a wrong layer.
        if(l.blurRadius)return -95;
        if(!l.height || uint64_t(l.stride)>~uint64_t{0}/4/l.height)return -22;
        if(l.stride>GPU_SCENE_MAX_DIMENSION || l.height>GPU_SCENE_MAX_DIMENSION)return -7;
        const uint64_t bytes=uint64_t(l.stride)*l.height*4;
        if(!bytes || bytes>l.bytes || bytes>DesktopLayerMaxBytes)return -7;
        if(capture && (skipMask&bit)){if(o.pixels || o.capacity)return -22;}
        else if(capture && (!o.pixels || o.capacity<bytes || o.pixels>~uint64_t{0}-bytes))return -22;
        // Only bytes this capture copies count.
        if(capture && !((skipMask|kept|occluded)&bit)){
            if(bytes>options.copyLimit-copying || copying>options.copyLimit)return -7;
            copying+=bytes;
        }
        o.incarnation=l.incarnation;o.generation=l.generation;o.bytes=bytes;
        o.window=l.window;o.width=l.width;o.height=l.height;o.stride=l.stride;
        o.clientWidth=l.clientWidth;o.clientHeight=l.clientHeight;o.clientX=l.clientX;o.clientY=l.clientY;
        o.upscale=l.upscale;o.flags=l.legacyOpaque?BROOK_DESKTOP_LAYER_LEGACY_OPAQUE:0;
        if(kept&bit)o.flags|=BROOK_DESKTOP_LAYER_KEPT;
        if(occluded&bit)o.flags|=BROOK_DESKTOP_LAYER_OCCLUDED;
        if(l.opacity!=255)o.flags|=BROOK_DESKTOP_LAYER_HAS_OPACITY | (uint32_t(l.opacity)<<BROOK_DESKTOP_LAYER_OPACITY_SHIFT);
    }
    if(capture)for(uint32_t i=0;i<source.count;++i)for(uint32_t j=0;j<i;++j){
        if((skipMask&(uint32_t{1}<<i)) || (skipMask&(uint32_t{1}<<j)))continue;
        const auto& a=wire.layers[i];const auto& b=wire.layers[j];
        if(a.pixels<b.pixels+b.bytes && b.pixels<a.pixels+a.bytes)return -22;
    }
    if(!current(source))return -11;
    if(capture)for(uint32_t i=0;i<source.count;++i){
        if(skipMask&(uint32_t{1}<<i))continue;
        const auto& l=source.layers[i];const auto& o=wire.layers[i];
        if(kept&(uint32_t{1}<<i)){++tally.keeps;tally.bytesKept+=o.bytes;continue;}
        if(occluded&(uint32_t{1}<<i)){++tally.occluded;tally.bytesOccluded+=o.bytes;continue;}
        ++tally.captures;tally.bytesCopied+=o.bytes;if(keepMask&(uint32_t{1}<<i))++tally.keepMisses;
        for(uint64_t offset=0;offset<o.bytes;){
            const uint64_t chunk=o.bytes-offset<chunkLimit?o.bytes-offset:chunkLimit;
            if(!sink(l,offset,o.pixels+offset,chunk))return -14;
            offset+=chunk;
        }
    }
    if(!current(source))return -11;
    if(counters && capture){++counters->calls;counters->keeps+=tally.keeps;counters->keepMisses+=tally.keepMisses;
        counters->captures+=tally.captures;counters->bytesCopied+=tally.bytesCopied;counters->bytesKept+=tally.bytesKept;
        counters->occluded+=tally.occluded;counters->bytesOccluded+=tally.bytesOccluded;}
    wire.count=source.count;return 0;
}
}

#pragma once
#include "window.h"
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
// Snapshot is retained by the caller. Sink reads a bounded range of pinned pages and
// copies to a user destination; current rechecks the actual window state.
// A failed capture must never publish its metadata as a completed frame.
template<class Sink,class Current>
int64_t ExportDesktopLayers(const WmDesktopLayers& source,BrookDesktopLayers& wire,
                            bool capture,Sink sink,Current current,uint64_t chunkLimit=4096,uint32_t skipMask=0) {
    constexpr uint64_t maxBytes=64ull*1024*1024;
    if(!chunkLimit || chunkLimit>65536)return -22;
    if(wire.version!=BROOK_DESKTOP_LAYERS_VERSION || wire.reserved || source.count>BROOK_DESKTOP_LAYER_LIMIT)return -22;
    if(capture && wire.count!=source.count)return -11;
    if(skipMask && (!capture || (source.count<32 && (skipMask>>source.count))))return -22;
    if(skipMask)for(uint32_t i=0;i<source.count;++i)
        if(wire.layers[i].window!=source.layers[i].window || wire.layers[i].incarnation!=source.layers[i].incarnation)return -11;
    uint64_t total=0;
    for(uint32_t i=0;i<source.count;++i){
        const auto& l=source.layers[i];auto& o=wire.layers[i];
        // This renderer cannot reproduce backdrop blur; leave the OS-owned
        // software path in charge instead of presenting a wrong layer.
        if(l.blurRadius)return -95;
        if(!l.height || uint64_t(l.stride)>~uint64_t{0}/4/l.height)return -22;
        const uint64_t bytes=uint64_t(l.stride)*l.height*4;
        if(!bytes || bytes>l.bytes || bytes>maxBytes-total)return -7;
        total+=bytes;
        if(capture && (skipMask&(uint32_t{1}<<i))){if(o.pixels || o.capacity)return -22;}
        else if(capture && (!o.pixels || o.capacity<bytes || o.pixels>~uint64_t{0}-bytes))return -22;
        o.incarnation=l.incarnation;o.generation=l.generation;o.bytes=bytes;
        o.window=l.window;o.width=l.width;o.height=l.height;o.stride=l.stride;
        o.clientWidth=l.clientWidth;o.clientHeight=l.clientHeight;o.clientX=l.clientX;o.clientY=l.clientY;
        o.upscale=l.upscale;o.flags=l.legacyOpaque?BROOK_DESKTOP_LAYER_LEGACY_OPAQUE:0;
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
        for(uint64_t offset=0;offset<o.bytes;){
            const uint64_t chunk=o.bytes-offset<chunkLimit?o.bytes-offset:chunkLimit;
            if(!sink(l,offset,o.pixels+offset,chunk))return -14;
            offset+=chunk;
        }
    }
    if(!current(source))return -11;
    wire.count=source.count;return 0;
}
}

#pragma once
#include <stdint.h>

namespace brook {
// Pure pixel helpers for debug screenshots of Broadwell primary plane A
// (BdwScanoutCaptureLive). No hardware access: the caller resolves the live
// surface to its pages and supplies pageOf(index) -> CPU address or nullptr.

// DSPACNTR pixel format (bits 29:26) for the formats a capture understands.
constexpr uint32_t BdwCaptureFormatXrgb8888=6,BdwCaptureFormatXbgr8888=14;
struct BdwCaptureLayout {
    uint32_t width=0,height=0,stride=0;bool tiled=false,swapRb=false;
    uint32_t pages=0; // pages the image spans
};
// Validates the plane registers for a capture of width x height pixels.
inline bool BdwCaptureLayoutFrom(uint32_t control,uint32_t stride,uint32_t offset,uint32_t width,uint32_t height,
                                 BdwCaptureLayout& out){
    out={};
    const uint32_t format=(control>>26)&15u;
    const bool tiled=(control&(1u<<10))!=0;
    if(!(control&(1u<<31)) || (format!=BdwCaptureFormatXrgb8888 && format!=BdwCaptureFormatXbgr8888) || offset ||
       !width || !height || width>4096 || height>2160 || stride<width*4 || stride>32768 || (stride%64) ||
       (tiled && (stride%512)))return false;
    out.width=width;out.height=height;out.stride=stride;out.tiled=tiled;out.swapRb=format==BdwCaptureFormatXbgr8888;
    out.pages=tiled?((height+7)/8)*(stride/512):uint32_t((uint64_t(height-1)*stride+uint64_t(width)*4+4095)/4096);
    return true;
}
// Copies the surface into packed B,G,R,X rows of layout.width pixels. Gen8 X
// tiles are 4 KiB: 8 rows of 512 bytes, tiles row-major across the stride,
// with no CPU-visible bit-6 swizzle on Broadwell. Returns false when a page
// is missing (destination contents are then unspecified).
template<class PageOf>
bool BdwCaptureCopy(uint32_t* destination,const BdwCaptureLayout& l,PageOf pageOf){
    // Every offset and length is a multiple of four bytes.
    auto copy=[](uint8_t* d,const uint8_t* s,uint32_t n){
        auto* d32=reinterpret_cast<uint32_t*>(d);auto* s32=reinterpret_cast<const uint32_t*>(s);
        for(uint32_t i=0;i<n/4;++i)d32[i]=s32[i];
    };
    if(l.tiled){
        const uint32_t tilesPerRow=l.stride/512;
        for(uint32_t ty=0;ty*8<l.height;++ty)
            for(uint32_t tx=0;tx<tilesPerRow && tx*128<l.width;++tx){
                const uint8_t* tile=pageOf(ty*tilesPerRow+tx);
                if(!tile)return false;
                const uint32_t n=l.width-tx*128<128?l.width-tx*128:128;
                for(uint32_t k=0;k<8 && ty*8+k<l.height;++k)
                    copy(reinterpret_cast<uint8_t*>(destination+uint64_t(ty*8+k)*l.width+tx*128),tile+k*512,n*4);
            }
    }else{
        for(uint32_t y=0;y<l.height;++y){
            uint64_t at=uint64_t(y)*l.stride;uint32_t left=l.width*4;
            auto* out=reinterpret_cast<uint8_t*>(destination+uint64_t(y)*l.width);
            while(left){
                const uint8_t* page=pageOf(uint32_t(at>>12));
                if(!page)return false;
                const uint32_t in=uint32_t(at&4095),n=left<4096-in?left:4096-in;
                copy(out,page+in,n);out+=n;at+=n;left-=n;
            }
        }
    }
    if(l.swapRb)
        for(uint64_t i=0,n=uint64_t(l.width)*l.height;i<n;++i){
            const uint32_t p=destination[i];destination[i]=(p&0xff00ff00u)|((p>>16)&0xffu)|((p&0xffu)<<16);
        }
    return true;
}
// CURPOS: sign-magnitude, X in bits 15:0, Y in bits 31:16.
inline void BdwCaptureCursorPosition(uint32_t position,int32_t& x,int32_t& y){
    x=int32_t(position&0x7fffu);if(position&0x8000u)x=-x;
    y=int32_t((position>>16)&0x7fffu);if(position&0x80000000u)y=-y;
}
// Composites a 64x64 premultiplied ARGB cursor image at (x, y).
inline void BdwCaptureCursorComposite(uint32_t* destination,uint32_t width,uint32_t height,
                                      const uint32_t* pixels,int32_t x,int32_t y){
    for(int32_t row=0;row<64;++row){
        const int32_t dy=y+row;if(dy<0 || dy>=int32_t(height))continue;
        for(int32_t col=0;col<64;++col){
            const int32_t dx=x+col;if(dx<0 || dx>=int32_t(width))continue;
            const uint32_t s=pixels[row*64+col],a=s>>24;if(!a)continue;
            uint32_t& d=destination[uint64_t(dy)*width+uint32_t(dx)];
            auto mix=[&](unsigned shift){
                const uint32_t v=((s>>shift)&255u)+(((d>>shift)&255u)*(255u-a)+127)/255;return (v>255?255u:v)<<shift;
            };
            d=0xff000000u|mix(16)|mix(8)|mix(0);
        }
    }
}
}

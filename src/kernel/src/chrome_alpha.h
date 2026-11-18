#pragma once
#include <stdint.h>
#include "chrome_material.h"

namespace brook {
// One alpha rule for window chrome composited as a separate straight-alpha
// layer: the kernel GPU (virgl) chrome layer and the chrome surfaces captured
// for VKRENDER. Painters leave alpha unset on opaque fills, so after painting
// every decoration pixel (the outer rectangle minus the client rectangle) is
// made opaque and the client interior keeps its alpha (transparent). A block
// of overlaid caption buttons on a client-decorated window is opaque as a
// whole. Glass titlebars on the virgl path additionally take the material
// alpha (compositor.cpp); that rule leaves once chrome surfaces carry their
// material (refactor stage 1.3). A caption block matched to a client's title
// strip keeps its rounded top-right corner transparent.
struct ChromeAlphaRect {int64_t x,y,w,h;};

// OR alpha 255 into every pixel of r inside the width x height buffer.
inline void ChromeMarkOpaque(uint32_t* pixels,uint64_t stride,uint32_t width,uint32_t height,ChromeAlphaRect r){
    int64_t left=r.x,top=r.y,right=r.x+r.w,bottom=r.y+r.h;
    if(left<0)left=0;
    if(top<0)top=0;
    if(right>int64_t(width))right=width;
    if(bottom>int64_t(height))bottom=height;
    if(!pixels || left>=right || top>=bottom)return;
    for(int64_t y=top;y<bottom;++y){
        uint32_t* row=pixels+uint64_t(y)*stride;
        for(int64_t x=left;x<right;++x)row[x]|=0xff000000u;
    }
}
// The decoration: outer minus client, as up to four bands.
inline void ChromeMarkFrameOpaque(uint32_t* pixels,uint64_t stride,uint32_t width,uint32_t height,
                                  ChromeAlphaRect outer,ChromeAlphaRect client){
    const int64_t ox1=outer.x+outer.w,oy1=outer.y+outer.h,cx1=client.x+client.w,cy1=client.y+client.h;
    ChromeMarkOpaque(pixels,stride,width,height,{outer.x,outer.y,outer.w,client.y-outer.y});      // top border + title
    ChromeMarkOpaque(pixels,stride,width,height,{outer.x,cy1,outer.w,oy1-cy1});                   // bottom border
    ChromeMarkOpaque(pixels,stride,width,height,{outer.x,client.y,client.x-outer.x,client.h});    // left border
    ChromeMarkOpaque(pixels,stride,width,height,{cx1,client.y,ox1-cx1,client.h});                 // right border
}
// An overlaid caption block: opaque, except the rounded top-right corner of
// a client-matched strip (radius 0: square). Corner pixels are left as they
// are; the caption painters skip them, so they stay transparent.
inline void ChromeMarkCaptionBlockOpaque(uint32_t* pixels,uint64_t stride,uint32_t width,uint32_t height,
                                         ChromeAlphaRect block,unsigned radius){
    for(int64_t row=0;row<block.h;++row){
        const int64_t inset=row<int64_t(radius)?int64_t(ChromeCornerInset(radius,unsigned(row))):0;
        ChromeMarkOpaque(pixels,stride,width,height,{block.x,block.y+row,block.w-inset,1});
    }
}
} // namespace brook

#pragma once
#include <stdint.h>
#include "chrome_material.h"

namespace brook {
// Preserve the transparent client hole when forcing a square opaque frame
// for a renderer budget, or when writing a client-matched caption block.
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
// Give every pixel of r that the painter left without alpha the alpha a
// (painted alpha is kept).
inline void ChromeFillAlpha(uint32_t* pixels,uint64_t stride,uint32_t width,uint32_t height,ChromeAlphaRect r,uint32_t a){
    int64_t left=r.x,top=r.y,right=r.x+r.w,bottom=r.y+r.h;
    if(left<0)left=0;
    if(top<0)top=0;
    if(right>int64_t(width))right=width;
    if(bottom>int64_t(height))bottom=height;
    if(!pixels || left>=right || top>=bottom)return;
    for(int64_t y=top;y<bottom;++y){
        uint32_t* row=pixels+uint64_t(y)*stride;
        for(int64_t x=left;x<right;++x)if(!(row[x]>>24))row[x]=(row[x]&0x00ffffffu)|a<<24;
    }
}
// Clip freshly painted, explicit-alpha chrome, affecting only the pixels
// at each corner. The client stays rectangular; the bottom radius is
// confined to the frame border.
inline void ChromeMaskFrameCorners(uint32_t* pixels,uint64_t stride,uint32_t width,uint32_t height,
                                   ChromeAlphaRect outer,const ChromeMaterialCache& corners,bool focused){
    const int64_t ox=outer.x,oy=outer.y,ow=outer.w,oh=outer.h;
    if(ow<int64_t(corners.top.radius*2) || oh<int64_t(corners.top.radius*2))return;
    for(unsigned corner=0;corner<4;++corner){
        // Client textures remain rectangular. Bottom masks stop within the
        // owned border rather than pretending to clip client pixels.
        const ChromeCornerMask& mask=corner<2?corners.top:corners.bottom;
        for(unsigned row=0;row<mask.radius;++row)for(unsigned col=0;col<mask.radius;++col){
            const int64_t y=corner<2?oy+int64_t(row):oy+oh-1-int64_t(row);
            const int64_t x=(corner&1)?ox+ow-1-int64_t(col):ox+int64_t(col);
            if(x<0||y<0||x>=int64_t(width)||y>=int64_t(height))continue;
            uint32_t& pixel=pixels[uint64_t(y)*stride+uint64_t(x)];
            const uint32_t rim=corners.rims[focused?1:0][corner<2?(row<mask.radius/2?0:1):2];
            pixel=ChromeMaskedRim(pixel,mask.coverage[row][col],mask.inner[row][col],rim);
        }
    }
}
} // namespace brook

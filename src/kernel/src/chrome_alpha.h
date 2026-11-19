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
// The glass frame (stage 1.5: shared by virgl's chrome layer and the chrome
// captured for a renderer that can blur): the borders opaque, the title bar
// at the material alpha, title text (fg) and the accent stroke opaque again
// (they are painted without alpha), and, unless corners is null, the four
// corners rounded with the material's masks and rims. topBorder: rows of
// solid border above the title bar.
inline void ChromeMarkFrameGlass(uint32_t* pixels,uint64_t stride,uint32_t width,uint32_t height,
                                 ChromeAlphaRect outer,ChromeAlphaRect client,int64_t topBorder,uint32_t materialAlpha,
                                 uint32_t fg,uint32_t accent,const ChromeMaterialCache* corners,bool focused){
    if(!pixels)return;
    const int64_t ox=outer.x,oy=outer.y,ow=outer.w,oh=outer.h,cx=client.x,cy=client.y,cw=client.w,ch=client.h;
    ChromeFillAlpha(pixels,stride,width,height,{ox,oy,ow,topBorder},0xff);
    ChromeFillAlpha(pixels,stride,width,height,{ox,oy+topBorder,ow,(cy-oy)-topBorder},materialAlpha);
    ChromeFillAlpha(pixels,stride,width,height,{ox,cy+ch,ow,(oy+oh)-(cy+ch)},0xff);
    ChromeFillAlpha(pixels,stride,width,height,{ox,cy,cx-ox,ch},0xff);
    ChromeFillAlpha(pixels,stride,width,height,{cx+cw,cy,(ox+ow)-(cx+cw),ch},0xff);
    fg&=0xffffff;accent&=0xffffff;
    for(int64_t y=oy+topBorder;y<cy;++y)for(int64_t x=ox;x<ox+ow;++x){
        if(x<0||y<0||x>=int64_t(width)||y>=int64_t(height))continue;
        uint32_t& pixel=pixels[uint64_t(y)*stride+uint64_t(x)];
        const uint32_t rgb=pixel&0xffffff;
        if(rgb==fg||rgb==accent)pixel|=0xff000000u;
    }
    if(!corners || ow<int64_t(corners->top.radius*2) || oh<int64_t(corners->top.radius*2))return;
    for(unsigned corner=0;corner<4;++corner){
        // Client textures remain rectangular. Bottom masks stop within the
        // owned border rather than pretending to clip client pixels.
        const ChromeCornerMask& mask=corner<2?corners->top:corners->bottom;
        for(unsigned row=0;row<mask.radius;++row)for(unsigned col=0;col<mask.radius;++col){
            const int64_t y=corner<2?oy+int64_t(row):oy+oh-1-int64_t(row);
            const int64_t x=(corner&1)?ox+ow-1-int64_t(col):ox+int64_t(col);
            if(x<0||y<0||x>=int64_t(width)||y>=int64_t(height))continue;
            uint32_t& pixel=pixels[uint64_t(y)*stride+uint64_t(x)];
            const uint32_t rim=corners->rims[focused?1:0][corner<2?(row<mask.radius/2?0:1):2];
            pixel=ChromeMaskedRim(pixel,mask.coverage[row][col],mask.inner[row][col],rim);
        }
    }
}
} // namespace brook

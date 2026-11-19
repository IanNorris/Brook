// chrome_alpha.h against the two loops it replaced (copied here as oracles):
// the VKRENDER chrome-capture span loop and the opaque (non-glass) branch of
// the virgl chrome layer's per-strip alpha. Random frames, clients, offsets
// and clipping; exact equality of every pixel.
#include "../../kernel/src/chrome_alpha.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"CHROME_ALPHA line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using namespace brook;

static void oracleCapture(uint32_t* pixels,uint32_t width,uint32_t height,int cx,int cy,int cw,int ch){
    auto clip=[](int64_t v,uint32_t limit)->uint32_t {return v<=0?0:v>=limit?limit:static_cast<uint32_t>(v);};
    const uint32_t left=clip(cx,width),right=clip(int64_t(cx)+cw,width);
    const uint32_t top=clip(cy,height),bottom=clip(int64_t(cy)+ch,height);
    for(uint32_t y=0;y<height;++y){
        uint32_t* row=pixels+uint64_t(y)*width;
        if(y<top || y>=bottom){for(uint32_t x=0;x<width;++x)row[x]|=0xff000000u;}
        else{for(uint32_t x=0;x<left;++x)row[x]|=0xff000000u;for(uint32_t x=right;x<width;++x)row[x]|=0xff000000u;}
    }
}
static void oracleLayer(uint32_t* buf,uint32_t stride,uint32_t fbW,uint32_t fbH,int ox,int oy,int ow,int oh,int cx,int cy,int cw,int ch,int border){
    auto setAlpha=[&](int x,int y,int rw,int rh){
        if(rw<=0 || rh<=0)return;
        if(x<0){rw+=x;x=0;}
        if(y<0){rh+=y;y=0;}
        if(x+rw>(int)fbW)rw=(int)fbW-x;
        if(y+rh>(int)fbH)rh=(int)fbH-y;
        for(int yy=y;yy<y+rh;++yy){uint32_t* row=buf+(uint32_t)yy*stride;for(int xx=x;xx<x+rw;++xx)row[xx]=(row[xx]&0x00FFFFFFu)|0xff000000u;}
    };
    const int titleBarH=(cy-oy)-border;
    setAlpha(ox,oy,ow,border);setAlpha(ox,oy+border,ow,titleBarH);
    setAlpha(ox,cy+ch,ow,(oy+oh)-(cy+ch));setAlpha(ox,cy,cx-ox,ch);setAlpha(cx+cw,cy,(ox+ow)-(cx+cw),ch);
}

// The overlaid caption-block loops (virgl layer and VKRENDER capture) with
// the client-matched rounded top-right corner.
static void oracleBlockLayer(uint32_t* buf,uint32_t stride,uint32_t fbW,uint32_t fbH,int bx,int by,int bw,int bh,unsigned radius){
    for(int y=by;y<by+bh;++y){
        if(y<0 || y>=int(fbH))continue;
        uint32_t* row=buf+uint32_t(y)*stride;
        const int end=bx+bw-int(ChromeCornerInset(radius,unsigned(y-by)));
        for(int x=bx;x<end;++x)if(x>=0 && x<int(fbW))row[x]|=0xFF000000u;
    }
}
static void oracleBlockCapture(uint32_t* pixels,uint32_t width,uint32_t height,unsigned radius){
    for(uint64_t i=0;i<uint64_t(width)*height;++i)pixels[i]|=0xff000000u;
    for(unsigned row=0;row<radius && row<height;++row)
        for(unsigned x=width-(ChromeCornerInset(radius,row)<width?ChromeCornerInset(radius,row):width);x<width;++x)
            pixels[uint64_t(row)*width+x]=0;
}

// The glass branch of virgl's chrome layer before stage 1.5 moved it into
// chrome_alpha.h (ChromeMarkFrameGlass), as the oracle for that move.
static void oracleGlass(uint32_t* buf,uint32_t stride,uint32_t fbW,uint32_t fbH,int ox,int oy,int ow,int oh,
                        int cx,int cy,int cw,int ch,int topBorderH,uint32_t opacity,uint32_t fgColor,uint32_t accentColor,
                        const ChromeMaterialCache* material,bool focused){
    auto setAlpha = [&](int x, int y, int rw, int rh, uint32_t a,bool preserve=false) {
        if (rw <= 0 || rh <= 0) return;
        if (x < 0) { rw += x; x = 0; }
        if (y < 0) { rh += y; y = 0; }
        if (x + rw > (int)fbW)  rw = (int)fbW  - x;
        if (y + rh > (int)fbH) rh = (int)fbH - y;
        uint32_t aShift = a << 24;
        for (int yy = y; yy < y + rh; ++yy)
        {
            uint32_t* row = buf + (uint32_t)yy * stride;
            for (int xx = x; xx < x + rw; ++xx) if(!preserve || !(row[xx]>>24))row[xx] = (row[xx] & 0x00FFFFFFu) | aShift;
        }
    };
    int titleBarH  = (cy - oy) - topBorderH;
    setAlpha(ox, oy, ow, topBorderH, 0xFF,true);
    setAlpha(ox, oy + topBorderH, ow, titleBarH, opacity,true);
    setAlpha(ox, cy + ch, ow, (oy + oh) - (cy + ch), 0xFF,true);
    setAlpha(ox, cy, cx - ox, ch, 0xFF,true);
    setAlpha(cx + cw, cy, (ox + ow) - (cx + cw), ch, 0xFF,true);
    {
        const uint32_t fg=fgColor;
        const uint32_t accent=accentColor&0xffffff;
        for(int y=oy+topBorderH;y<cy;++y)for(int x=ox;x<ox+ow;++x) {
            if(x<0||y<0||x>=int(fbW)||y>=int(fbH))continue;
            auto& pixel=buf[uint64_t(y)*stride+x];
            const uint32_t rgb=pixel&0xffffff;
            if(rgb==fg||rgb==accent)pixel|=0xff000000;
        }
    }
    if(material) {
        if(ow>=int(material->top.radius*2)&&oh>=int(material->top.radius*2))
        for(unsigned corner=0;corner<4;++corner){
            const auto& mask=corner<2?material->top:material->bottom;
            for(unsigned row=0;row<mask.radius;++row)for(unsigned col=0;col<mask.radius;++col){
                const int y=corner<2?oy+int(row):oy+oh-1-int(row);
                const int x=(corner&1)?ox+ow-1-int(col):ox+int(col);
                if(x<0||y<0||x>=int(fbW)||y>=int(fbH))continue;
                auto& pixel=buf[uint64_t(y)*stride+x];
                const uint32_t rim=material->rims[focused][corner<2?(row<mask.radius/2?0:1):2];
                pixel=ChromeMaskedRim(pixel,mask.coverage[row][col],mask.inner[row][col],rim);
            }
        }
    }
}

int main(){
    uint32_t rng=0x2545F491u;
    auto next=[&](uint32_t n){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return n?rng%n:0u;};
    for(int iter=0;iter<3000;++iter){
        // Capture: buffer is the outer frame at the origin.
        {
            const uint32_t w=1+next(40),h=1+next(40);
            const int border=next(4),title=next(12);
            const int cx=border,cy=border+title,cw=int(w)-2*border,ch=int(h)-cy-border;
            std::vector<uint32_t> a(w*h),b(w*h);for(auto& p:a)p=next(0x01000000u);b=a;
            oracleCapture(a.data(),w,h,cx,cy,cw,ch);
            ChromeMarkFrameOpaque(b.data(),w,w,h,{0,0,w,h},{cx,cy,cw,ch});
            CHECK(a==b);
        }
        // Layer: frame anywhere on a framebuffer (partly off-screen too).
        {
            const uint32_t fbW=8+next(60),fbH=8+next(60),stride=fbW+next(5);
            const int border=1+next(3),title=next(10),cw=1+next(30),ch=1+next(30);
            const int ox=int(next(fbW+20))-10,oy=int(next(fbH+20))-10;
            const int cx=ox+border,cy=oy+border+title,ow=cw+2*border,oh=(cy-oy)+ch+border;
            std::vector<uint32_t> a(stride*fbH),b;for(auto& p:a)p=next(0x01000000u);b=a;
            oracleLayer(a.data(),stride,fbW,fbH,ox,oy,ow,oh,cx,cy,cw,ch,border);
            ChromeMarkFrameOpaque(b.data(),stride,fbW,fbH,{ox,oy,ow,oh},{cx,cy,cw,ch});
            CHECK(a==b);
        }
    }
    for(int iter=0;iter<2000;++iter){
        const unsigned radius=next(3)?next(20):0;
        // Layer: anywhere on the framebuffer, prior contents kept in the corner.
        {
            const uint32_t fbW=8+next(80),fbH=8+next(60),stride=fbW+next(5);
            const int bw=1+next(60),bh=1+next(40),bx=int(next(fbW+20))-10,by=int(next(fbH+20))-10;
            std::vector<uint32_t> a(stride*fbH),b;for(auto& p:a)p=next(0x01000000u);b=a;
            oracleBlockLayer(a.data(),stride,fbW,fbH,bx,by,bw,bh,radius);
            ChromeMarkCaptionBlockOpaque(b.data(),stride,fbW,fbH,{bx,by,bw,bh},radius);
            CHECK(a==b);
        }
        // Capture: the block is the whole cleared surface; painters leave the
        // corner untouched, so it starts (and must end) transparent.
        {
            const uint32_t w=1+next(60),h=1+next(40);
            std::vector<uint32_t> a(w*h);for(auto& p:a)p=next(0x01000000u);
            for(unsigned row=0;row<radius && row<h;++row)
                for(unsigned x=w-(ChromeCornerInset(radius,row)<w?ChromeCornerInset(radius,row):w);x<w;++x)a[row*w+x]=0;
            std::vector<uint32_t> b=a;
            oracleBlockCapture(a.data(),w,h,radius);
            ChromeMarkCaptionBlockOpaque(b.data(),w,w,h,{0,0,w,h},radius);
            CHECK(a==b);
        }
    }
    // Glass frames (virgl's layer, and stage 1.5's capture for a renderer
    // that can blur): exact against the old loop, with and without corners,
    // on a framebuffer (partly off-screen) and as a capture at the origin.
    for(int iter=0;iter<3000;++iter){
        ChromeMaterialCache material;
        const uint32_t fg=0x00e5edf8u,accent=0x0068acffu;
        const int border=1+next(3),title=4+next(20),cw=1+next(60),ch=1+next(40);
        material.Ensure(0x0028466fu,0x00253851u,40+next(200),title,next(8),border);
        const bool capture=next(2),focused=next(2),corners=next(2);
        const uint32_t fbW=capture?uint32_t(cw+2*border):8+next(90),fbH=capture?uint32_t(border+title+ch+border):8+next(70);
        const uint32_t stride=capture?fbW:fbW+next(5);
        const int ox=capture?0:int(next(fbW+20))-10,oy=capture?0:int(next(fbH+20))-10;
        const int cx=ox+border,cy=oy+border+title,ow=cw+2*border,oh=(cy-oy)+ch+border;
        std::vector<uint32_t> a(stride*fbH),b;
        // Painted pixels: some with alpha, some the text or accent colour.
        for(auto& p:a){const uint32_t pick=next(8);p=pick==0?fg:pick==1?accent:pick==2?(next(256)<<24|next(0x01000000u)):next(0x01000000u);}
        b=a;
        const uint32_t opacity=next(256);
        oracleGlass(a.data(),stride,fbW,fbH,ox,oy,ow,oh,cx,cy,cw,ch,border,opacity,fg,accent,corners?&material:nullptr,focused);
        ChromeMarkFrameGlass(b.data(),stride,fbW,fbH,{ox,oy,ow,oh},{cx,cy,cw,ch},border,opacity,fg,accent,
                             corners?&material:nullptr,focused);
        CHECK(a==b);
    }
    // A block (overlaid captions) is opaque as a whole, clipped.
    std::vector<uint32_t> px(10*6,0x00123456u);
    ChromeMarkOpaque(px.data(),10,10,6,{-2,4,5,9});
    for(int y=0;y<6;++y)for(int x=0;x<10;++x)CHECK((px[y*10+x]>>24)==((y>=4 && x<3)?255u:0u));
    std::puts("CHROME_ALPHA PASS");
}

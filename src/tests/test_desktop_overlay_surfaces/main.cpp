// Per-surface overlay cache (stage 1.4): drawing only the planned surfaces
// over the cached buffer must reproduce a full redraw onto a zero buffer, for
// any sequence of surfaces (overlapping, moving, changing keys, appearing and
// leaving) and composited pointers. Band keys from surface keys must change
// exactly when a band's pixels can.
#include "../../kernel/src/desktop_overlay_surfaces.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"OVERLAY_SURFACES line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using namespace brook;
static constexpr uint32_t W=40,H=30;
static uint32_t rng=12345;
static uint32_t next(){rng=rng*1664525u+1013904223u;return rng>>8;}
// A surface whose pixels depend on what was under it: drawing it onto
// uncleared pixels gives a different result, so the oracle catches a missing
// clear as well as a missing redraw.
static void draw(uint32_t* p,const DesktopOverlaySurfaceSlot& s){
    for(int64_t y=s.rect.y;y<int64_t(s.rect.y)+s.rect.height;++y)for(int64_t x=s.rect.x;x<int64_t(s.rect.x)+s.rect.width;++x){
        if(x<0||y<0||x>=W||y>=H)continue;uint32_t& d=p[y*W+x];
        d=d*31u+uint32_t(s.key*2654435761u)+uint32_t(x*7+y*13)+s.kind;}
}
static void zero(uint32_t* p,const DesktopOverlayRect& r){
    for(int64_t y=r.y;y<int64_t(r.y)+r.height;++y)for(int64_t x=r.x;x<int64_t(r.x)+r.width;++x)
        if(x>=0&&y>=0&&x<W&&y<H)p[y*W+x]=0;
}
int main(){
    static uint32_t buffer[W*H],reference[W*H];
    DesktopOverlaySurfaceCache cache{};
    unsigned kept=0,drawn=0;
    DesktopOverlaySurfaceSlot current[4]{};uint32_t count=0;
    for(unsigned step=0;step<200000;++step){
        // Mutate the scene a little: most steps change one surface or nothing.
        const uint32_t op=next()%10;
        if(op==0 && count<4){current[count++]={next()%5,{int32_t(next()%W)-5,int32_t(next()%H)-5,next()%20+1,next()%15+1},next()%3+1};}
        else if(op==1 && count){const uint32_t i=next()%count;for(uint32_t j=i;j+1<count;++j)current[j]=current[j+1];--count;}
        else if(op==2 && count){current[next()%count].key=next()%3+1;}
        else if(op==3 && count){auto& r=current[next()%count].rect;r.x+=int32_t(next()%3)-1;r.y+=int32_t(next()%3)-1;}
        else if(op==4 && count){current[next()%count].key=0;}
        const auto plan=DesktopOverlayPlanSurfaces(cache,current,count);
        if(plan.wipe)std::memset(buffer,0,sizeof(buffer));
        for(uint32_t i=0;i<plan.clearCount;++i)zero(buffer,plan.clears[i]);
        for(uint32_t i=0;i<count;++i){if(plan.draw>>i&1u){draw(buffer,current[i]);++drawn;}else ++kept;}
        // Unknown content is never kept.
        for(uint32_t i=0;i<count;++i)if(!current[i].key)CHECK(plan.draw>>i&1u);
        DesktopOverlayCommitSurfaces(cache,current,count);
        std::memset(reference,0,sizeof(reference));
        for(uint32_t i=0;i<count;++i)draw(reference,current[i]);
        CHECK(!std::memcmp(buffer,reference,sizeof(buffer)));
        // Sometimes a pointer is composited over the result (debug capture).
        if(next()%7==0){const DesktopOverlayRect c{int32_t(next()%W),int32_t(next()%H),4,4};
            for(int y=c.y;y<c.y+4&&y<int(H);++y)for(int x=c.x;x<c.x+4&&x<int(W);++x)buffer[y*W+x]^=0xabcdef;
            DesktopOverlayAddForeign(cache,c);}
        if(next()%5000==0){cache={};std::memset(buffer,0x5a,sizeof(buffer));} // unknown buffer: wipe
    }
    CHECK(kept>drawn/4); // the cache is actually used
    // Band keys: unchanged surfaces keep their band key; a changed surface
    // changes only its own band's key; unkeyed rectangles hash pixels.
    {
        static uint32_t img[64*48];
        const DesktopOverlayRect rects[2]={{0,40,64,8},{2,10,20,28}};
        uint64_t keys[2]={11,22};
        DesktopOverlayBands a{},b{};
        CHECK(DesktopOverlayMakeBands(64,48,rects,2,a,3u));CHECK(DesktopOverlayMakeBands(64,48,rects,2,b,3u));
        DesktopOverlayKeyBandsFromRects(img,64,48,a,rects,keys,2);
        img[45*64+3]=7; // pixels are not consulted for keyed bands
        DesktopOverlayKeyBandsFromRects(img,64,48,b,rects,keys,2);
        CHECK(a.count==2 && a.keys[0]==b.keys[0] && a.keys[1]==b.keys[1] && a.keys[0]!=a.keys[1] && a.keys[0] && a.keys[1]);
        keys[0]=12;DesktopOverlayKeyBandsFromRects(img,64,48,b,rects,keys,2);
        const unsigned taskbar=a.bands[0].y==40?0:1,launcher=1-taskbar;
        CHECK(b.keys[taskbar]!=a.keys[taskbar] && b.keys[launcher]==a.keys[launcher]);
        // A pointer rectangle without a key: its band follows its pixels.
        const DesktopOverlayRect withCursor[3]={rects[0],rects[1],{30,42,4,4}};
        uint64_t cursorKeys[3]={12,22,0};
        DesktopOverlayBands c{},d{};
        CHECK(DesktopOverlayMakeBands(64,48,withCursor,3,c,3u));CHECK(DesktopOverlayMakeBands(64,48,withCursor,3,d,3u));
        DesktopOverlayKeyBandsFromRects(img,64,48,c,withCursor,cursorKeys,3);
        img[43*64+31]^=1;
        DesktopOverlayKeyBandsFromRects(img,64,48,d,withCursor,cursorKeys,3);
        bool changed=false;
        for(uint32_t i=0;i<c.count;++i){const bool hasCursor=c.bands[i].y<=43 && c.bands[i].y+c.bands[i].height>43;
            if(hasCursor){CHECK(c.keys[i]!=d.keys[i]);changed=true;}else CHECK(c.keys[i]==d.keys[i]);}
        CHECK(changed);
    }
    std::printf("OVERLAY_SURFACES PASS kept=%u drawn=%u\n",kept,drawn);
}

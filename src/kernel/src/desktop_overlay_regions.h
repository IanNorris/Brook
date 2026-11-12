#pragma once
#include <stdint.h>

namespace brook {
struct DesktopOverlayRect {int32_t x,y;uint32_t width,height;};
struct DesktopOverlayBand {uint32_t x,y,width,height;};
struct DesktopOverlayBands {DesktopOverlayBand bands[7]{};uint32_t count=0;};

// Enumerate a packed band's bounded reads without retaining a user pointer.
// The callback receives offsets into the immutable full image and destination.
template<class Copy>
inline bool DesktopOverlayPackedRead(const DesktopOverlayBand& b,uint32_t width,uint32_t height,
                                     uint32_t chunk,Copy copy){
    if(!width || !height || !chunk || !b.width || !b.height || b.x>=width || b.y>=height ||
       b.width>width-b.x || b.height>height-b.y)return false;
    const uint64_t rowBytes=uint64_t(b.width)*4;
    if(!b.x && b.width==width){
        const uint64_t base=uint64_t(b.y)*width*4,total=rowBytes*b.height;
        for(uint64_t pos=0;pos<total;){const uint32_t n=uint32_t(total-pos<chunk?total-pos:chunk);
            if(!copy(base+pos,pos,n))return false;pos+=n;}
    }else for(uint32_t y=0;y<b.height;++y){
        const uint64_t base=(uint64_t(b.y+y)*width+b.x)*4;
        for(uint64_t pos=0;pos<rowBytes;){const uint32_t n=uint32_t(rowBytes-pos<chunk?rowBytes-pos:chunk);
            if(!copy(base+pos,uint64_t(y)*rowBytes+pos,n))return false;pos+=n;}
    }
    return true;
}

// Up to four semantic rectangles become at most seven non-overlapping row
// bands. A band may contain transparent pixels between its objects, but no
// nontransparent pixel in any admitted rectangle is omitted or drawn twice.
inline bool DesktopOverlayMakeBands(uint32_t width,uint32_t height,const DesktopOverlayRect* rects,
                                    uint32_t count,DesktopOverlayBands& out){
    out={};if(!width || !height || count>4 || (count && !rects))return false;
    struct Clipped {uint32_t left,top,right,bottom;};Clipped clipped[4]{};
    uint32_t n=0,edges[8]{},edgeCount=0;
    for(uint32_t i=0;i<count;++i){
        const int64_t left=rects[i].x,top=rects[i].y;
        const int64_t right=left+rects[i].width,bottom=top+rects[i].height;
        const int64_t l=left<0?0:left,t=top<0?0:top;
        const int64_t r=right>width?width:right,b=bottom>height?height:bottom;
        if(r<=l || b<=t)continue;
        clipped[n++]={uint32_t(l),uint32_t(t),uint32_t(r),uint32_t(b)};
        edges[edgeCount++]=uint32_t(t);edges[edgeCount++]=uint32_t(b);
    }
    for(uint32_t i=1;i<edgeCount;++i){uint32_t v=edges[i],j=i;while(j && edges[j-1]>v){edges[j]=edges[j-1];--j;}edges[j]=v;}
    for(uint32_t i=0;i+1<edgeCount;++i){
        const uint32_t top=edges[i],bottom=edges[i+1];if(top==bottom)continue;
        uint32_t left=width,right=0;
        for(uint32_t j=0;j<n;++j)if(clipped[j].top<=top && clipped[j].bottom>=bottom){
            if(clipped[j].left<left)left=clipped[j].left;
            if(clipped[j].right>right)right=clipped[j].right;
        }
        if(left>=right)continue;
        if(out.count && out.bands[out.count-1].x==left && out.bands[out.count-1].width==right-left &&
           out.bands[out.count-1].y+out.bands[out.count-1].height==top){out.bands[out.count-1].height+=bottom-top;continue;}
        if(out.count>=7)return false;
        out.bands[out.count++]={left,top,right-left,bottom-top};
    }
    return true;
}
}

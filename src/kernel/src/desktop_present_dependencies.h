#pragma once
#include <stdint.h>

namespace brook {
// Tracks pixels written by accepted older tickets after a newer ticket was
// reserved. A newer partial frame must cover their union, or be rejected.
// Bounded overflow and malformed input fail closed to a full presentation.
constexpr uint32_t DesktopDependencyLimit=32;
struct DesktopDependencyRect { uint32_t x,y,width,height; };
struct DesktopPresentDependencies {
    DesktopDependencyRect rects[DesktopDependencyLimit];
    uint32_t count;
    bool full;
};
// The renderer chooses its baseline before preparing a frame. Admission must
// prove that no other ticket presented between that choice and reservation;
// zero is not a valid previously accepted partial-frame baseline.
inline bool DesktopDependencyBaselineCurrent(uint64_t requested,uint64_t lastAccepted) {
    return requested && requested==lastAccepted;
}
inline bool DesktopDependencyValid(DesktopDependencyRect r) {
    return r.width && r.height && uint64_t(r.x)+r.width<=(uint64_t{1}<<32) &&
           uint64_t(r.y)+r.height<=(uint64_t{1}<<32);
}
inline void DesktopDependenciesFull(DesktopPresentDependencies& d) { d.full=true; d.count=0; }
inline void DesktopDependenciesAdd(DesktopPresentDependencies& d,const DesktopDependencyRect* r,uint32_t count) {
    if(d.full)return;
    if(!r || !count || count>DesktopDependencyLimit-d.count){DesktopDependenciesFull(d);return;}
    for(uint32_t i=0;i<count;++i)if(!DesktopDependencyValid(r[i])){DesktopDependenciesFull(d);return;}
    for(uint32_t i=0;i<count;++i)d.rects[d.count++]=r[i];
}
// Test coverage against the union of proposed regions, not merely against a
// single containing rectangle. At each y boundary, greedily extend the x
// interval; coverage cannot change until another rectangle's y edge.
inline bool DesktopDependenciesCovered(const DesktopPresentDependencies& d,const DesktopDependencyRect* covers,uint32_t count) {
    if(d.full || count>DesktopDependencyLimit || (count && !covers))return false;
    for(uint32_t i=0;i<count;++i)if(!DesktopDependencyValid(covers[i]))return false;
    for(uint32_t i=0;i<d.count;++i){
        const auto r=d.rects[i];if(!DesktopDependencyValid(r))return false;
        const uint64_t right=uint64_t(r.x)+r.width,bottom=uint64_t(r.y)+r.height;
        for(uint64_t y=r.y;y<bottom;){
            uint64_t nextY=bottom;
            for(uint32_t j=0;j<count;++j){const auto c=covers[j];const uint64_t end=uint64_t(c.y)+c.height;
                if(c.y>y && c.y<nextY)nextY=c.y;
                if(end>y && end<nextY)nextY=end;
            }
            uint64_t x=r.x;
            while(x<right){uint64_t reach=x;
                for(uint32_t j=0;j<count;++j){const auto c=covers[j];const uint64_t end=uint64_t(c.x)+c.width;
                    if(c.y<=y && uint64_t(c.y)+c.height>y && c.x<=x && end>reach)reach=end;
                }
                if(reach==x)return false;
                x=reach;
            }
            y=nextY;
        }
    }
    return true;
}
}

#include "../kernel/src/desktop_present_dependencies.h"
#include "../kernel/src/desktop_frame_scenes.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace brook;
static bool hit(DesktopDependencyRect r,uint32_t x,uint32_t y){
    return x>=r.x && y>=r.y && uint64_t(x)<uint64_t(r.x)+r.width && uint64_t(y)<uint64_t(r.y)+r.height;
}
int main(){
    assert(DesktopDependencyBaselineCurrent(10,10));
    assert(!DesktopDependencyBaselineCurrent(0,0));
    assert(!DesktopDependencyBaselineCurrent(10,11));
    DesktopPresentDependencies d{};
    DesktopDependencyRect target{0,0,8,8}, split[]={{0,0,4,8},{4,0,4,8}};
    DesktopDependenciesAdd(d,&target,1);
    assert(DesktopDependenciesCovered(d,split,2));
    split[1].x=5;assert(!DesktopDependenciesCovered(d,split,2));
    DesktopDependenciesFull(d);assert(!DesktopDependenciesCovered(d,&target,1));
    d={};DesktopDependencyRect invalid{UINT32_MAX,0,2,1};DesktopDependenciesAdd(d,&invalid,1);assert(d.full);
    d={};DesktopDependencyRect many[DesktopDependencyLimit+1]{};DesktopDependenciesAdd(d,many,DesktopDependencyLimit+1);assert(d.full);
    int scenes[3]{};DesktopFrameScenes<int,3> frames{};
    assert(frames.Insert({1,10,1,1,1,0,false,&scenes[0],false}));
    assert(frames.Insert({1,11,2,1,1,0,false,&scenes[1],false}));
    assert(frames.Insert({2,12,3,1,1,0,false,&scenes[2],false}));
    frames.RecordAccepted(1,10,&target,1,false);
    assert(frames.Find(1,10)->dependencies.count==0);
    assert(frames.Find(1,11)->dependencies.count==1);
    assert(frames.Find(2,12)->dependencies.count==0);
    frames.RecordAccepted(1,10,nullptr,0,true);
    assert(frames.Find(1,11)->dependencies.full);
    assert(frames.Remove(1,11)==&scenes[1]);
    assert(frames.Insert({1,13,4,1,1,0,false,&scenes[1],false}));
    assert(!frames.Find(1,13)->dependencies.full && !frames.Find(1,13)->dependencies.count);
    uint32_t random=0xa561b327;
    auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned trial=0;trial<30000;++trial){
        d={};DesktopDependencyRect required[5],provided[5];const unsigned nr=1+next()%5,np=next()%6;
        auto rect=[&](){return DesktopDependencyRect{next()%12,next()%9,1+next()%5,1+next()%5};};
        for(unsigned i=0;i<nr;++i)required[i]=rect();
        for(unsigned i=0;i<np;++i)provided[i]=rect();
        DesktopDependenciesAdd(d,required,nr);bool oracle=true;
        for(uint32_t y=0;y<14;++y)for(uint32_t x=0;x<17;++x){bool need=false,have=false;
            for(unsigned i=0;i<nr;++i)need|=hit(required[i],x,y);
            for(unsigned i=0;i<np;++i)have|=hit(provided[i],x,y);
            if(need && !have)oracle=false;
        }
        assert(DesktopDependenciesCovered(d,provided,np)==oracle);
    }
    puts("DESKTOP_PRESENT_DEPENDENCIES_PASS");
}

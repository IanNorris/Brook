#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "desktop_metrics.h"
using namespace brook;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}} while(0)
int main() {
    CHECK(!DesktopMetricsFor(0)&&!DesktopMetricsFor(125)&&!DesktopMetricsFor(201));
    for(unsigned from:{100u,150u,200u})for(unsigned to:{100u,150u,200u}) {
        const auto& old=*DesktopMetricsFor(from);const auto& next=*DesktopMetricsFor(to);
        CHECK(next.pixels(-17)==-next.pixels(17));
        DesktopWindowGeometry original{180,180,800,600,false};
        auto changed=ReframeDesktopWindow(original,old,next,1920,1080-next.taskbar());
        CHECK(original.clientX(old)==changed.clientX(next));
        CHECK(original.clientY(old)==changed.clientY(next));
        CHECK(changed.clientW==800&&changed.clientH==600);
        auto restored=ReframeDesktopWindow(changed,next,old,1920,1080-old.taskbar());
        CHECK(restored.x==original.x&&restored.y==original.y);
        for(int i=0;i<3;++i) {
            auto r=DesktopCaptionRect(changed,next,i);
            CHECK(r.contains(r.x,r.y)&&r.contains(r.x+r.w-1,r.y+r.h-1));
            CHECK(!r.contains(r.x-1,r.y)&&!r.contains(r.x+r.w,r.y));
            CHECK(!r.contains(r.x,r.y+r.h));
            for(int j=i+1;j<3;++j){auto other=DesktopCaptionRect(changed,next,j);CHECK(!other.contains(r.x,r.y));}
        }
        original.clientDecorated=true;
        changed=ReframeDesktopWindow(original,old,next,1920,1080-next.taskbar());
        CHECK(changed.x==original.x&&changed.y==original.y);
        CHECK(DesktopCaptionRect(changed,next,0).w==0);
        auto top=ReframeDesktopWindow({0,0,800,600,false},old,next,1920,1080-next.taskbar());
        CHECK(top.y>=0);
        auto small=DesktopCaptionRect({0,0,80,40,false},next,0);
        CHECK(small.w*3<=80);
        auto edge=ReframeDesktopWindow({4000,4000,800,600,false},old,next,1920,1080-next.taskbar());
        CHECK(edge.y+next.title()+2*next.border()<=1080-next.taskbar());
        CHECK(edge.x<=1920-next.captionButton());
        for(int screen:{40,320}) {
            auto narrow=ReframeDesktopWindow({180,180,800,600,false},old,next,screen,600);
            auto close=DesktopCaptionRect(narrow,next,0);
            CHECK(close.x<screen && close.x+close.w>0);
        }
    }
    std::puts("desktop metrics: scale transitions, physical client origins, CSD, captions and screen bounds PASS");
}

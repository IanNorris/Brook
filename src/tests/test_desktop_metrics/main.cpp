#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "desktop_metrics.h"
#include "scene_damage.h"
#include "desktop_pointer_route.h"
using namespace brook;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}} while(0)
static void checkDesktopPointerRouting() {
    DesktopPointerGate gate;
    using Event=DesktopPointerEvent;
    CHECK(gate.consume(Event::Down,0,true));
    CHECK(gate.consume(Event::Up,0,false)); // menu closed on press
    CHECK(!gate.consume(Event::Up,0,false)); // ownership cleared exactly once
    CHECK(!gate.consume(Event::Down,0,false));
    CHECK(!gate.consume(Event::Up,0,false));
    CHECK(gate.consume(Event::Move,0,true));
    CHECK(gate.consume(Event::Scroll,1,true));
    CHECK(!gate.consume(Event::Move,0,false));
    CHECK(gate.consume(Event::Down,1,true));
    CHECK(!gate.consume(Event::Down,0,false)); // unrelated button remains client-owned
    CHECK(gate.consume(Event::Up,1,false));
    CHECK(!gate.consume(Event::Up,0,false));
    CHECK(gate.consume(Event::Down,32,true)); // invalid index cannot shift out of range
    CHECK(!gate.consume(Event::Up,32,false));
}
static bool contains(const SceneRect& r,int x,int y) {
    return r.left<=x && x<r.right && r.top<=y && y<r.bottom;
}
static void checkSceneDamage() {
    SceneRect native=SceneClientBlitRect(10,20,320,200,100,80,4,1920,1080);
    CHECK(native.left==10 && native.top==20 && native.right==110 && native.bottom==100);
    native=SceneClientBlitRect(-10,-20,320,200,100,80,1,1920,1080);
    CHECK(native.left==0 && native.top==0 && native.right==90 && native.bottom==60);
    CHECK(SceneClientBlitRect(2000,0,320,200,100,80,1,1920,1080).empty());
    // Deliberately reverse the overlap chain: a single pass misses layer 0.
    SceneRect layers[]={{0,0,4,4},{3,0,7,4},{6,0,10,4},{12,0,16,4}};
    SceneRect result=SceneDamageClosure(layers[2],layers,4);
    CHECK(result.left==0 && result.right==10 && result.top==0 && result.bottom==4);
    CHECK(!result.intersects(layers[3]));
    CHECK(!SceneDamageClosure({},layers,4).intersects(layers[0]));
    CHECK(!layers[0].intersects({4,0,8,4})); // touching edges are disjoint
    SceneRect clipped=SceneRect{-20,-10,30,15}.clipped(16,8);
    CHECK(clipped.left==0 && clipped.top==0 && clipped.right==16 && clipped.bottom==8);

    // Compare repeated partial alpha composition with a fresh full scene.
    // A shadow is deliberately unchanged while a lower layer updates, and a
    // fourth disjoint window must retain its exact pixels. Wrong one-pass
    // closure or blending onto the previous scene fails this oracle.
    unsigned char full[64]{},partial[64]{},value[]={200,120,0,80};
    auto paint=[&](unsigned char* pixels,SceneRect damage,bool fresh) {
        for(int y=0;y<4;++y)for(int x=0;x<16;++x)
            if(fresh || contains(damage,x,y))pixels[y*16+x]=240;
        for(int i=0;i<4;++i) {
            if(!fresh && !damage.intersects(layers[i]))continue;
            for(int y=0;y<4;++y)for(int x=0;x<16;++x)if(contains(layers[i],x,y))
                pixels[y*16+x]=static_cast<unsigned char>((unsigned(pixels[y*16+x])*3+value[i])/4);
        }
    };
    paint(full,{},true);paint(partial,{},true);
    for(int frame=0;frame<1000;++frame) {
        int changed=frame%3;
        value[changed]=static_cast<unsigned char>((frame*31)&255);
        paint(full,{},true);
        result=SceneDamageClosure(layers[changed],layers,4);
        paint(partial,result,false);
        for(int pixel=0;pixel<64;++pixel)CHECK(full[pixel]==partial[pixel]);
    }
}
int main() {
    checkSceneDamage();
    checkDesktopPointerRouting();
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

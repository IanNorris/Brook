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
    uint32_t opaque[]={0xff112233u,0xff445566u,0xff778899u};
    CHECK(SceneRowOpaque(opaque,3));opaque[1]=0x80445566u;
    CHECK(!SceneRowOpaque(opaque,3) && SceneRowOpaque(opaque,1));
    uint32_t visible[3]={0xdeadbeefu,0xdeadbeefu,0xdeadbeefu};
    unsigned restored=0;
    const auto restore=[&] {
        ++restored;
        for (auto& pixel:visible) pixel=0xff0000ffu;
    };
    const uint32_t complete[]={0xff112233u,0xff445566u,0xff778899u};
    SceneBlitWindowRow(visible,complete,3,restore);
    CHECK(!restored && visible[0]==complete[0] && visible[2]==complete[2]);
    const uint32_t mixed[]={0,0x80ff0000u,0xff778899u};
    SceneBlitWindowRow(visible,mixed,3,restore);
    CHECK(restored==1 && visible[0]==0xff0000ffu && visible[1]==0xff80007fu && visible[2]==mixed[2]);
    SceneBlitWindowRow(visible,mixed,3,[]{});
    CHECK(restored==1 && visible[1]==0xffc0003fu);
    const SceneRect client{0,0,3,6};
    CHECK(client.covers(SceneRect{1,1,3,5}));
    CHECK(!client.covers(SceneRect{-1,1,3,5}) && !client.covers({}));
    SceneRect native=SceneClientBlitRect(10,20,320,200,100,80,4,1920,1080);
    CHECK(native.left==10 && native.top==20 && native.right==110 && native.bottom==100);
    native=SceneClientBlitRect(-10,-20,320,200,100,80,1,1920,1080);
    CHECK(native.left==0 && native.top==0 && native.right==90 && native.bottom==60);
    CHECK(SceneClientBlitRect(2000,0,320,200,100,80,1,1920,1080).empty());
    CHECK(SceneClientBlitRect(0,1081,320,200,320,200,1,1920,1080).empty());
    CHECK(SceneClientBlitRect(1920,0,320,200,320,200,1,1920,1080).empty());
    CHECK(SceneClientBlitRect(-400,0,320,200,320,200,1,1920,1080).empty());
    native=SceneClientBlitRect(1900,1070,320,200,320,200,1,1920,1080);
    CHECK(native.left==1900 && native.top==1070 && native.right==1920 && native.bottom==1080);
    // Deliberately reverse the overlap chain: a single pass misses layer 0.
    SceneRect layers[]={{0,0,4,4},{3,0,7,4},{6,0,10,4},{12,0,16,4}};
    SceneRect result=SceneDamageClosure(layers[2],layers,4);
    CHECK(result.left==0 && result.right==10 && result.top==0 && result.bottom==4);
    CHECK(!result.intersects(layers[3]));
    CHECK(!SceneDamageClosure({},layers,4).intersects(layers[0]));
    {   // D3: a clipped (CSD) layer is not pulled in whole; its caption overlay is, once reached.
        const SceneRect big{0,0,1000,1000},win{100,100,200,200};
        const SceneRect ls[2]={big,win};const bool expand[2]={false,true};
        const SceneRect extra[2]={SceneRect{900,0,1000,40},SceneRect{}};
        SceneRect r=SceneDamageClosureLayered(SceneRect{150,150,160,160},ls,expand,extra,2);
        CHECK(r.left==100 && r.top==100 && r.right==200 && r.bottom==200);   // the chromed window, not the big client
        r=SceneDamageClosureLayered(SceneRect{950,30,960,60},ls,expand,extra,2);
        CHECK(r.left==900 && r.top==0 && r.right==1000 && r.bottom==60);      // reached the overlay: it joins
    }
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
    CHECK(!DesktopMetricsFor(0)&&!DesktopMetricsFor(123)&&!DesktopMetricsFor(201));
    for (unsigned scale=100;scale<=200;scale+=5) {
        const auto* metrics=DesktopMetricsFor(scale);
        CHECK(metrics && metrics->percent==scale && metrics->title()==int(40*scale/100));
    }
    CHECK(DesktopMetricsFor(120)->taskbar()==58 && DesktopMetricsFor(135)->taskbar()==65);
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
        CHECK(DesktopCaptionOverlayRect(changed,next).w==0);
        // Overlaid captions sit inside the client surface, right of its shadow inset.
        DesktopWindowGeometry overlay{100,50,800,600,true,true,12,8};
        auto block=DesktopCaptionOverlayRect(overlay,next);
        CHECK(block.w==3*next.captionButton()&&block.h==next.title());
        CHECK(block.x+block.w==100+800-12&&block.y==50+8);
        for(int i=0;i<3;++i) {
            auto r=DesktopCaptionRect(overlay,next,i);
            CHECK(r.w==next.captionButton()&&block.contains(r.x,r.y)&&block.contains(r.x+r.w-1,r.y+r.h-1));
            for(int j=i+1;j<3;++j){auto other=DesktopCaptionRect(overlay,next,j);CHECK(!other.contains(r.x,r.y));}
        }
        CHECK(DesktopCaptionRect(overlay,next,0).x>DesktopCaptionRect(overlay,next,2).x);
        auto tight=DesktopCaptionOverlayRect({0,0,60,20,true,true,0,0},next);
        CHECK(tight.w<=60&&tight.h<=20);
        CHECK(DesktopCaptionOverlayRect({0,0,60,20,true,true,0,20},next).w==0);
        // A client title strip taller than the native title: the block spans it.
        DesktopWindowGeometry strip{100,50,800,600,true,true,12,8,next.pixels(40)};
        auto tall=DesktopCaptionOverlayRect(strip,next);
        CHECK(tall.h==next.pixels(40)&&tall.y==58&&tall.w==block.w);
        for(int i=0;i<3;++i)CHECK(DesktopCaptionRect(strip,next,i).h==next.pixels(40));
        CHECK(DesktopCaptionOverlayRect({0,0,300,30,true,true,0,0,40},next).h==30);
        // Client-matched style: contrast ink, packed corner radius.
        CHECK(DesktopCaptionInk(0xd3e3fd)==0x1f1f1f && DesktopCaptionInk(0x202124)==0xe8eaed);
        CHECK(DesktopCaptionInk(0xffffff)==0x1f1f1f && DesktopCaptionInk(0x000000)==0xe8eaed);
        CHECK(DesktopCaptionStyleRgb(DesktopCaptionStyleValid|(10u<<25)|0x123456)==0x123456);
        CHECK(DesktopCaptionStyleRadius(DesktopCaptionStyleValid|(10u<<25)|0x123456)==10);
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
    std::puts("desktop metrics: scale transitions, physical client origins, CSD, captions, caption overlay and screen bounds PASS");
}

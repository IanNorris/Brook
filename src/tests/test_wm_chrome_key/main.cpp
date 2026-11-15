// Change-based window chrome: the kernel content key and the renderer's
// keep/recapture protocol (tools/vulkan-compositor-pkg/desktop_chrome_cache.h).
#include "wm_chrome_key.h"
#include "desktop_chrome_cache.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace brook;

namespace brook { WmChromeCaptureStats g_chromeCaptureStats{}; }

struct FakeWin {
    char title[64]="Quake II",appId[128]="yquake2";
    uint64_t identity=7,metrics=0x1000,incarnation=42;
    uint32_t clientW=1280,clientH=720,border=2,titleH=32,state=0;
    bool focused=true,visible=true,noChrome=false;uint64_t overlay=0;
    int32_t hover=-1;uint32_t theme=1,appearance=0,scale=100;uint64_t settings=3,launcher=5;
    int32_t x=100,y=80; // position: never part of the key
    uint32_t outerW()const{return clientW+2*border;}
    uint32_t outerH()const{return clientH+titleH+2*border;}
};

static uint64_t Key(const FakeWin& w){
    WmChromeKeyInput in{};
    in.title=w.title;in.titleCapacity=sizeof(w.title);in.appId=w.appId;in.appIdCapacity=sizeof(w.appId);
    in.windowIdentity=w.identity;in.metricsIdentity=w.metrics;
    in.outerWidth=w.outerW();in.outerHeight=w.outerH();in.clientWidth=w.clientW;in.clientHeight=w.clientH;
    in.clientOffsetX=int32_t(w.border);in.clientOffsetY=int32_t(w.titleH+w.border);in.state=w.state;
    in.focused=w.focused;in.visible=w.visible;in.noChrome=w.noChrome;in.captionOverlay=w.overlay;in.hoverButton=w.hover;
    in.theme=w.theme;in.appearance=w.appearance;in.scale=w.scale;in.settingsEpoch=w.settings;in.launcherEpoch=w.launcher;
    return WmChromeKey(in);
}

static void KeyChanges(){
    FakeWin base;const uint64_t k=Key(base);
    assert(k && (k&1));
    { FakeWin w=base;assert(Key(w)==k); }                                   // no change
    { FakeWin w=base;w.x+=300;w.y-=40;assert(Key(w)==k); }                   // move: same pixels
    { FakeWin w=base;std::strcpy(w.title,"Quake II - map");assert(Key(w)!=k); }  // title
    { FakeWin w=base;w.focused=false;assert(Key(w)!=k); }                    // focus
    { FakeWin w=base;w.clientW=1281;assert(Key(w)!=k); }                     // resize width
    { FakeWin w=base;w.clientH=721;assert(Key(w)!=k); }                      // resize height
    { FakeWin w=base;w.hover=0;assert(Key(w)!=k);FakeWin v=base;v.hover=2;assert(Key(v)!=Key(w)); } // hover close vs minimise
    { FakeWin w=base;w.state=1;assert(Key(w)!=k); }                          // maximised icon
    { FakeWin w=base;w.theme=2;assert(Key(w)!=k); }                          // theme
    { FakeWin w=base;w.scale=150;assert(Key(w)!=k); }                        // scale
    { FakeWin w=base;w.launcher=6;assert(Key(w)!=k); }                       // icon snapshot
    { FakeWin w=base;w.identity=8;assert(Key(w)!=k); }                       // recycled slot
    { FakeWin w=base;std::strcpy(w.appId,"other");assert(Key(w)!=k); }       // icon identity
    // Overlaid caption buttons: mode and the client's shadow insets move the block.
    { FakeWin w=base;w.noChrome=true;const uint64_t csd=Key(w);w.overlay=1ull<<63;const uint64_t o=Key(w);
      assert(o!=csd);w.overlay|=12;assert(Key(w)!=o);FakeWin v=w;v.overlay|=8u<<16;assert(Key(v)!=Key(w));
      FakeWin h=w;h.overlay|=40ull<<32;assert(Key(h)!=Key(w)); }
    // Adjacent strings cannot alias: ("ab","c") vs ("a","bc").
    { FakeWin a=base,b=base;std::strcpy(a.title,"ab");std::strcpy(a.appId,"c");
      std::strcpy(b.title,"a");std::strcpy(b.appId,"bc");assert(Key(a)!=Key(b)); }
    // Bytes after the terminator are not part of the title.
    { FakeWin a=base,b=base;std::strcpy(a.title,"x");std::strcpy(b.title,"x");b.title[5]='q';assert(Key(a)==Key(b)); }
}

// A model of the kernel side of KEYED capture and KEEP, plus the renderer's
// per-buffer decision, counting the bytes a frame copies.
struct Kernel {
    FakeWin win;std::vector<uint32_t> rendered;uint64_t copied=0,keeps=0,misses=0,captures=0;
    void render(){rendered.assign(size_t(win.outerW())*win.outerH(),0);
        for(size_t i=0;i<rendered.size();++i)rendered[i]=uint32_t(Key(win)>>(i%32))|0xff000000u;}
    BrookDesktopLayer meta(uint64_t pixels,uint64_t capacity){
        BrookDesktopLayer l{};l.pixels=pixels;l.capacity=capacity;l.incarnation=win.incarnation;l.generation=1;
        l.bytes=uint64_t(win.outerW())*win.outerH()*4;l.window=1;l.width=l.stride=l.clientWidth=win.outerW();
        l.height=l.clientHeight=win.outerH();l.clientX=win.x;l.clientY=win.y;l.upscale=1;return l;}
    int keyed(BrookDesktopLayers& wire,std::vector<uint32_t>& buffer){
        render();buffer=rendered;copied+=rendered.size()*4;++captures;
        wire.count=1;wire.layers[0]=meta(1,buffer.size()*4);wire.reserved=Key(win);return 0;}
    int keep(BrookDesktopLayers& wire){
        const auto& l=wire.layers[0];
        if(wire.reserved!=Key(win) || l.incarnation!=win.incarnation || l.width!=win.outerW() || l.height!=win.outerH()){++misses;return -116;}
        ++keeps;wire.layers[0]=meta(0,0);return 0;}
};

struct Renderer {
    DesktopChromeHeld held{};std::vector<uint32_t> buffer;uint64_t serial=1;
    void frame(Kernel& k){
        BrookDesktopLayers wire{};
        if(desktop_chrome_can_keep(&held,1,k.win.incarnation,serial)){
            desktop_chrome_keep_request(&held,&wire);
            assert(wire.layers[0].pixels==0 && wire.layers[0].capacity==0 && wire.reserved==held.key);
            if(!k.keep(wire))return;
            desktop_chrome_forget(&held);
        }
        wire={};wire.version=BROOK_DESKTOP_LAYERS_VERSION;
        const uint64_t need=uint64_t(k.win.outerW())*k.win.outerH()*4;
        if(buffer.size()*4<need){buffer.assign(need/4,0);++serial;} // reallocation: new serial
        k.keyed(wire,buffer);desktop_chrome_hold(&held,&wire,serial);
    }
};

static void KeepProtocol(){
    Kernel k;Renderer r;
    r.frame(k);assert(k.captures==1 && k.copied==uint64_t(k.win.outerW())*k.win.outerH()*4);
    const uint64_t first=k.copied;
    for(int i=0;i<60;++i)r.frame(k);                       // 60 unchanged frames
    assert(k.copied==first && k.keeps==60 && k.captures==1);
    assert(r.buffer==k.rendered);                          // held pixels are current
    k.win.x+=50;r.frame(k);assert(k.copied==first && k.keeps==61); // a move copies nothing
    std::strcpy(k.win.title,"Quake II - base1");r.frame(k); // title: one recapture
    assert(k.captures==2 && k.misses==1 && r.buffer==k.rendered);
    for(int i=0;i<10;++i)r.frame(k);assert(k.captures==2);
    k.win.focused=false;r.frame(k);assert(k.captures==3 && r.buffer==k.rendered);
    k.win.hover=0;r.frame(k);assert(k.captures==4);k.win.hover=-1;r.frame(k);assert(k.captures==5 && r.buffer==k.rendered);
    k.win.clientW+=64;r.frame(k);assert(k.captures==6 && r.buffer==k.rendered); // resize grows the buffer
    for(int i=0;i<10;++i)r.frame(k);assert(k.captures==6);
    k.win.incarnation++;r.frame(k);assert(k.captures==7);   // new incarnation: never keep
    r.serial++;r.frame(k);assert(k.captures==8);             // buffer reallocated elsewhere
    // Nothing held (no key, zero-byte chrome) never asks to keep.
    DesktopChromeHeld empty{};assert(!desktop_chrome_can_keep(&empty,1,k.win.incarnation,r.serial));
    BrookDesktopLayers none{};none.reserved=0;none.layers[0].bytes=100;desktop_chrome_hold(&empty,&none,3);
    assert(!empty.key);
}

int main(){
    KeyChanges();KeepProtocol();
    std::puts("test_wm_chrome_key: PASS");return 0;
}

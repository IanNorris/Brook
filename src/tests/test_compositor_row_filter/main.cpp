#include "../../kernel/src/compositor_row_filter.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace brook;

// A model of the software scanout: back buffer -> (shadow filter) -> framebuffer.
struct Screen {
    uint32_t w,h,stride;std::vector<uint32_t> back,fb,shadow;bool valid=false;uint64_t bytes=0;
    Screen(uint32_t w_,uint32_t h_,uint32_t s):w(w_),h(h_),stride(s),back(size_t(s)*h_),fb(size_t(s)*h_,0xdeadbeef),shadow(size_t(w_)*h_){}
    GopRowFilterResult flip(uint32_t minY,uint32_t maxY,bool full){
        if(!valid)full=true;
        if(full){minY=0;maxY=h;}
        auto r=GopRowFilterCopy(back.data(),stride,shadow.data(),w,w,minY,maxY,!full,[&](uint32_t y){
            std::memcpy(&fb[size_t(y)*stride],&back[size_t(y)*stride],size_t(w)*4);bytes+=uint64_t(w)*4;});
        if(full)valid=true;
        return r;
    }
    bool matches()const{for(uint32_t y=0;y<h;++y)if(std::memcmp(&fb[size_t(y)*stride],&back[size_t(y)*stride],size_t(w)*4))return false;return true;}
    void fill(uint32_t y0,uint32_t y1,uint32_t v){for(uint32_t y=y0;y<y1;++y)for(uint32_t x=0;x<w;++x)back[size_t(y)*stride+x]=v;}
};

static void RowEqual(){
    uint32_t a[5]={1,2,3,4,5},b[5]={1,2,3,4,5};
    for(uint32_t n=0;n<=5;++n)assert(GopRowEqual(a,b,n));
    b[4]=9;assert(GopRowEqual(a,b,4));assert(!GopRowEqual(a,b,5)); // odd tail word
    b[4]=5;b[0]=0;assert(!GopRowEqual(a,b,1));
}

static void FirstFlipCopiesEverything(){
    Screen s(17,9,20);s.fill(0,9,0x11);
    auto r=s.flip(3,4,false);                  // untrusted shadow: complete frame
    assert(r.copied==9 && r.skipped==0 && r.minY==0 && r.maxY==9 && s.matches());
}

static void IdleTaskbarRedrawCopiesNothing(){
    // The CPU path redraws the taskbar every frame and marks its rows dirty.
    Screen s(3200,64,3200);s.fill(0,48,0x202020);s.fill(48,64,0x303030);s.flip(0,64,true);
    const uint64_t before=s.bytes;
    for(int frame=0;frame<60;++frame){s.fill(48,64,0x303030);auto r=s.flip(48,64,false);assert(r.copied==0 && r.skipped==16 && r.minY>=r.maxY);}
    assert(s.bytes==before && s.matches());
}

static void ClockTickCopiesOnlyChangedRows(){
    Screen s(64,64,64);s.fill(0,64,0x1);s.flip(0,64,true);
    s.back[size_t(52)*64+60]=0xffffff;s.back[size_t(55)*64+61]=0xffffff;   // two glyph rows change
    auto r=s.flip(48,64,false);
    assert(r.copied==2 && r.skipped==14 && r.minY==52 && r.maxY==56 && s.matches());
}

static void LauncherOpenThenStatic(){
    Screen s(128,96,128);s.fill(0,96,0x5);s.flip(0,96,true);
    s.fill(10,60,0x77);auto r=s.flip(0,96,false);              // launcher appears
    assert(r.copied==50 && s.matches());
    r=s.flip(0,96,false);assert(r.copied==0 && r.skipped==96); // open and unchanged
}

static void ExternalWriterForcesFullRefresh(){
    // A leased renderer present wrote the framebuffer: the shadow is stale, so
    // the caller invalidates it and the next flip copies every row.
    Screen s(32,16,32);s.fill(0,16,0x9);s.flip(0,16,true);
    for(auto& px:s.fb)px=0x12345678;s.valid=false;
    auto r=s.flip(5,6,false);assert(r.copied==16 && s.matches());
    r=s.flip(0,16,false);assert(r.copied==0);
}

int main(){
    RowEqual();FirstFlipCopiesEverything();IdleTaskbarRedrawCopiesNothing();
    ClockTickCopiesOnlyChangedRows();LauncherOpenThenStatic();ExternalWriterForcesFullRefresh();
    std::puts("test_compositor_row_filter: OK");return 0;
}

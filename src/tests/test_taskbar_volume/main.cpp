#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "taskbar_volume.h"
using namespace brook;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}} while(0)

static void checkGlyph() {
    CHECK(TaskbarVolumeGlyphFor(0)==TaskbarVolumeGlyph::Muted);
    CHECK(TaskbarVolumeGlyphFor(BROOK_AUDIO_MUTED|80)==TaskbarVolumeGlyph::Muted);
    CHECK(TaskbarVolumeGlyphFor(1)==TaskbarVolumeGlyph::Low);
    CHECK(TaskbarVolumeGlyphFor(33)==TaskbarVolumeGlyph::Low);
    CHECK(TaskbarVolumeGlyphFor(34)==TaskbarVolumeGlyph::Medium);
    CHECK(TaskbarVolumeGlyphFor(66)==TaskbarVolumeGlyph::Medium);
    CHECK(TaskbarVolumeGlyphFor(67)==TaskbarVolumeGlyph::High);
    CHECK(TaskbarVolumeGlyphFor(100)==TaskbarVolumeGlyph::High);
}

static void checkWheel() {
    CHECK(TaskbarVolumeWheel(45,1)==50);
    CHECK(TaskbarVolumeWheel(45,-1)==40);
    CHECK(TaskbarVolumeWheel(47,1)==50);   // off-grid snaps to the next step
    CHECK(TaskbarVolumeWheel(47,-1)==45);
    CHECK(TaskbarVolumeWheel(100,1)==100); // clamped
    CHECK(TaskbarVolumeWheel(98,1)==100);
    CHECK(TaskbarVolumeWheel(0,-1)==0);
    CHECK(TaskbarVolumeWheel(3,-1)==0);
    CHECK(TaskbarVolumeWheel(45,120)==50); // magnitude ignored
    CHECK(TaskbarVolumeWheel(45,-120)==40);
    CHECK(TaskbarVolumeWheel(45,0)==45);
    CHECK(TaskbarVolumeWheel(BROOK_AUDIO_MUTED|45,1)==50);  // raising unmutes
    CHECK(TaskbarVolumeWheel(BROOK_AUDIO_MUTED|45,-1)==(BROOK_AUDIO_MUTED|40));
    for(uint32_t v=0;v<=100;++v){
        CHECK(brook_audio_control_valid(TaskbarVolumeWheel(v,1)));
        CHECK(brook_audio_control_valid(TaskbarVolumeWheel(v|BROOK_AUDIO_MUTED,-1)));
    }
    CHECK(TaskbarVolumeToggleMute(45)==(BROOK_AUDIO_MUTED|45));
    CHECK(TaskbarVolumeToggleMute(BROOK_AUDIO_MUTED|45)==45);
    CHECK(TaskbarVolumeClamp(-5)==0 && TaskbarVolumeClamp(250)==100 && TaskbarVolumeClamp(7)==7);
}

static void checkSlider() {
    const DesktopRect track{100,0,101,10};
    CHECK(TaskbarVolumeSliderValue(track,50)==0);
    CHECK(TaskbarVolumeSliderValue(track,100)==0);
    CHECK(TaskbarVolumeSliderValue(track,145)==45);
    CHECK(TaskbarVolumeSliderValue(track,200)==100);
    CHECK(TaskbarVolumeSliderValue(track,400)==100);
    CHECK(TaskbarVolumeSliderValue({0,0,0,0},5)==0);
    CHECK(TaskbarVolumeSliderValue({0,0,1,0},5)==0);
    const DesktopRect odd{10,0,137,10};
    for(uint32_t v=0;v<=100;++v)CHECK(TaskbarVolumeSliderValue(odd,TaskbarVolumeSliderX(odd,v))==v);
    CHECK(TaskbarVolumeSliderX(odd,250)==odd.x+odd.w-1);
}

static void checkPopup() {
    for(const DesktopMetrics* m:{&DesktopMetric100,&DesktopMetric150,&DesktopMetric200}){
        const int screenW=1280,taskbarY=720-m->taskbar();
        // Centred on an icon in the middle; kept on screen at the right edge.
        DesktopRect icon{600,taskbarY+4,m->pixels(18),m->pixels(36)};
        auto l=TaskbarVolumePopup(*m,icon,taskbarY,screenW,m->pixels(36));
        CHECK(std::abs((l.panel.x+l.panel.w/2)-(icon.x+icon.w/2))<=1);
        CHECK(l.panel.y+l.panel.h<=taskbarY);
        icon.x=screenW-m->pixels(20);
        l=TaskbarVolumePopup(*m,icon,taskbarY,screenW,m->pixels(36));
        CHECK(l.panel.x+l.panel.w<=screenW);
        CHECK(l.track.w>0 && l.mute.x+l.mute.w<l.track.x && l.track.x+l.track.w<l.value.x);
        CHECK(TaskbarVolumePopupHit(*m,l,l.mute.x+1,l.mute.y+1)==TaskbarVolumeHit::Mute);
        CHECK(TaskbarVolumePopupHit(*m,l,l.track.x+l.track.w/2,l.track.y+l.track.h/2)==TaskbarVolumeHit::Slider);
        CHECK(TaskbarVolumePopupHit(*m,l,l.track.x-2,l.track.y+1)==TaskbarVolumeHit::Slider); // knob overhang
        CHECK(TaskbarVolumePopupHit(*m,l,l.panel.x+l.panel.w-2,l.panel.y+1)==TaskbarVolumeHit::Panel);
        CHECK(TaskbarVolumePopupHit(*m,l,l.panel.x-1,l.panel.y)==TaskbarVolumeHit::Outside);
        CHECK(TaskbarVolumePopupHit(*m,l,icon.x+1,icon.y+1)==TaskbarVolumeHit::Outside); // icon closes
        // The build label beside the icon lifts the panel above it.
        const DesktopRect label{screenW-m->pixels(200),taskbarY-m->pixels(30),m->pixels(192),m->pixels(24)};
        l=TaskbarVolumePopup(*m,icon,taskbarY,screenW,m->pixels(36),&label);
        CHECK(l.panel.y+l.panel.h<=label.y && l.mute.y>l.panel.y);
        const DesktopRect far{0,taskbarY-m->pixels(30),m->pixels(50),m->pixels(24)};
        CHECK(TaskbarVolumePopup(*m,icon,taskbarY,screenW,m->pixels(36),&far).panel.y==
              TaskbarVolumePopup(*m,icon,taskbarY,screenW,m->pixels(36)).panel.y);
        // Narrow screens keep the left margin.
        l=TaskbarVolumePopup(*m,{0,taskbarY,10,10},taskbarY,screenW,m->pixels(36));
        CHECK(l.panel.x==m->pixels(4));
    }
}

static void checkTooltipBox() {
    const DesktopRect icon{1690,1040,16,36};
    auto b=TaskbarTooltipBox(icon,200,30,1036,4,1920,4);
    CHECK(b.x+b.w/2==icon.x+icon.w/2 && b.y+b.h+4==1036 && b.h==30 && b.w==200);
    b=TaskbarTooltipBox({1900,1040,16,36},200,30,1036,4,1920,4);CHECK(b.x+b.w==1916);
    b=TaskbarTooltipBox({0,1040,16,36},200,30,1036,4,1920,4);CHECK(b.x==4);
    b=TaskbarTooltipBox({0,10,16,36},200,30,10,4,300,4);CHECK(b.y==0);
}
static void checkTooltip() {
    char text[32];
    TaskbarVolumeTooltip(text,45,true);CHECK(!std::strcmp(text,"Volume: 45%"));
    TaskbarVolumeTooltip(text,100,true);CHECK(!std::strcmp(text,"Volume: 100%"));
    TaskbarVolumeTooltip(text,0,true);CHECK(!std::strcmp(text,"Volume: 0%"));
    TaskbarVolumeTooltip(text,7,true);CHECK(!std::strcmp(text,"Volume: 7%"));
    TaskbarVolumeTooltip(text,BROOK_AUDIO_MUTED|45,true);CHECK(!std::strcmp(text,"Muted"));
    TaskbarVolumeTooltip(text,45,false);CHECK(!std::strcmp(text,"No audio device"));
}

int main() {
    checkGlyph();checkWheel();checkSlider();checkPopup();checkTooltipBox();checkTooltip();
    std::puts("test_taskbar_volume: ok");
    return 0;
}

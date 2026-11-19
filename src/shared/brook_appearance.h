#ifndef BROOK_APPEARANCE_H
#define BROOK_APPEARANCE_H
#include <stdint.h>
/* Atomic desktop material preferences. Public ABI is one validated word. */
enum {
    BROOK_SYS_WM_APPEARANCE=0xB0B,
    BROOK_APPEARANCE_DARK=1u,
    BROOK_APPEARANCE_PERFORMANCE=2u,
    BROOK_APPEARANCE_DEFAULT=1u|(80u<<8)|(1u<<16)
};
/* 0xB02 op 4: what the compositor reports (stage 1.5). Acceleration is a
 * fact of the active backend, not a setting: Glass needs backdrop blur. */
enum {
    BROOK_THEME_OP_STATUS=4,
    BROOK_THEME_STATUS_GLASS_AVAILABLE=1u, /* the active backend can blur: Glass can be chosen */
    BROOK_THEME_STATUS_GLASS_ACTIVE=2u     /* the resolved theme is Glass */
};
static inline unsigned brook_appearance_opacity(uint32_t value){return (value>>8)&255;}
static inline unsigned brook_appearance_blur(uint32_t value){return (value>>16)&3;}
static inline int brook_appearance_valid(uint32_t value){
    return !(value&~0x0003ff03u) && brook_appearance_opacity(value)>=40 && brook_appearance_opacity(value)<=100;
}
static inline uint32_t brook_appearance_pack(int dark,int performance,unsigned opacity,unsigned blur){
    return (dark?1u:0u)|(performance?2u:0u)|(opacity<<8)|(blur<<16);
}
static inline unsigned brook_appearance_blur_strength(uint32_t value){
    static const unsigned strengths[]={0,2,8,16};return strengths[brook_appearance_blur(value)];
}
/* Strict bounded INI values. Malformed or unknown keys preserve the prior word. */
static inline uint32_t brook_appearance_setting(uint32_t prior,const char* line){
    const char* keys[]={"dark=","performance=","opacity=","blur="};
    for(unsigned i=0;i<4;++i){const char* a=line;const char* b=keys[i];
        while(*b && *a==*b){++a;++b;}if(*b)continue;
        unsigned value=0,digits=0;
        while(*a>='0'&&*a<='9'&&digits<3){value=value*10+(*a++-'0');++digits;}
        while(*a==' '||*a=='\t'||*a=='\r')++a;
        if(*a||!digits)return prior;
        uint32_t next=prior;
        if(i<2){if(value>1)return prior;next=(prior&~(1u<<i))|(value<<i);}
        else if(i==2){if(value<40||value>100)return prior;next=(prior&~0xff00u)|(value<<8);}
        else {if(value>3)return prior;next=(prior&~0x30000u)|(value<<16);}
        return next;
    }return prior;
}
/* App content colours (DESIGN.md "Content, by mode"): background, ink, dim
 * ink, line, raised, selection (accent at 11%/16% over the background), then
 * sunken, accent, its hover, and text on it. The hue drives all of them: a
 * Clearwater kernel reports the set in use with 0xB02 op 5, one colour per
 * call (a = BROOK_CONTENT_*, returns 0x00RRGGBB; older kernels EINVAL), and
 * apps poll it with the appearance word. brook_surface_palette() is the Blue
 * set, for older kernels. */
enum {
    BROOK_THEME_OP_CONTENT=5,
    BROOK_CONTENT_BACKGROUND=0, BROOK_CONTENT_INK, BROOK_CONTENT_DIM, BROOK_CONTENT_LINE, BROOK_CONTENT_RAISED,
    BROOK_CONTENT_SELECTION, BROOK_CONTENT_SUNKEN, BROOK_CONTENT_ACCENT, BROOK_CONTENT_ACCENT_HOVER,
    BROOK_CONTENT_ON_ACCENT, BROOK_CONTENT_COUNT
};
typedef struct BrookSurfacePalette {
    uint32_t content,ink,muted,line,raised,selection;
    uint32_t sunken,accent,accentHover,onAccent;
} BrookSurfacePalette;
static inline const BrookSurfacePalette* brook_surface_palette(int dark){
    static const BrookSurfacePalette palettes[]={
        {0xf5f8fc,0x1c2b3f,0x5c6d84,0xd7e0ed,0xffffff,0xdde8f7,0xeaf0f8,0x1c65cc,0x1959b4,0xffffff},
        {0x111c2b,0xe6edf7,0x93a4bb,0x26374f,0x18263a,0x1f334d,0x0c1522,0x6aaeff,0x7fb9ff,0x101e2e}
    };return &palettes[dark?1:0];
}
/* The palette's fields in BROOK_CONTENT_* order. */
static inline uint32_t* brook_surface_palette_field(BrookSurfacePalette* p,unsigned index){
    uint32_t* fields[BROOK_CONTENT_COUNT]={&p->content,&p->ink,&p->muted,&p->line,&p->raised,&p->selection,
                                           &p->sunken,&p->accent,&p->accentHover,&p->onAccent};
    return index<BROOK_CONTENT_COUNT?fields[index]:0;
}
#endif

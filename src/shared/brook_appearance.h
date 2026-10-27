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
typedef struct BrookSurfacePalette {uint32_t content,ink,muted,line,raised,selection;} BrookSurfacePalette;
static inline const BrookSurfacePalette* brook_surface_palette(int dark){
    static const BrookSurfacePalette palettes[]={
        {0xf4f7fc,0x25364b,0x65768b,0xdbe3ee,0xffffff,0xdce8fa},
        {0x172335,0xe5edf8,0xa4b2c7,0x32445d,0x213149,0x2c496d}
    };return &palettes[dark?1:0];
}
#endif

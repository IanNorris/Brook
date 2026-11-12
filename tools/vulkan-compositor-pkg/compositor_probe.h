#pragma once
#include <stdint.h>
#include <string.h>
#include "scene.h"

/* Deliberately small overlapping, clipped and scaled premultiplied scene. */
static inline void compositor_probe_prepare(uint32_t *v, uint32_t words, uint32_t variant) {
    for(uint32_t i=0;i<words;++i)v[i]=0xdeadbeefu;
    v[0]=64;v[1]=64;v[2]=2;v[3]=12288;
    uint32_t a[]={64,64,64,64,0,0,64,64,0,0,64,64,0,0,64,64,255};
    uint32_t b[]={4160,64,64,64,3,4,23,19,(uint32_t)-4,7,46,38,2,10,38,24,
                  variant%3==0?0:variant%3==1?127:255};
    memcpy(v+4,a,sizeof(a));memcpy(v+21,b,sizeof(b));
    for(uint32_t i=0;i<4096;++i){
        v[64+i]=0xff000000u | ((i*13u&255u)<<16) | ((i*7u&255u)<<8) | (i*3u&255u);
        uint32_t alpha=64u+(i%192u);
        v[4160+i]=(alpha<<24)|((alpha/2u)<<16)|((alpha/3u)<<8)|(alpha/4u);
    }
}
static inline uint32_t compositor_probe_scale(uint32_t a,uint32_t b) {return (a*b+127)/255;}
static inline uint32_t compositor_probe_pixel(const uint32_t *v,uint32_t x,uint32_t y) {
    uint32_t bg=v[64+y*64+x];
    if(x<2 || x>=40 || y<10 || y>=34 || x>=42 || y>=45)return bg;
    uint32_t sx=3+(2*(x+4)+1)*23/(2*46),sy=4+(2*(y-7)+1)*19/(2*38);
    uint32_t mode=v[21+16],opacity=mode&255u;
    uint32_t src=v[4160+sy*64+sx],alpha=compositor_probe_scale(src>>24,opacity),result=0;
    if(mode&BROOK_VK_OPAQUE_SOURCE)alpha=opacity;
    if(mode&BROOK_VK_STRAIGHT_ALPHA){
        result=0xff000000u;
        for(unsigned channel=0;channel<3;++channel){
            uint32_t s=(src>>(channel*8))&255u,d=(bg>>(channel*8))&255u;
            result|=((s*alpha+d*(255u-alpha)+127u)/255u)<<(channel*8);
        }
        return result;
    }
    for(unsigned channel=0;channel<4;++channel){
        uint32_t s=compositor_probe_scale((src>>(channel*8))&255,opacity);
        uint32_t d=(bg>>(channel*8))&255;
        uint32_t value=s+compositor_probe_scale(d,255-alpha);
        result|=(value>255?255:value)<<(channel*8);
    }
    return result;
}

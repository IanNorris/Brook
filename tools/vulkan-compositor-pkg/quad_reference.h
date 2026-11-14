#pragma once
/* CPU references for the quad compositor tests.
 *
 * quad_reference_pixel() is composite.comp's per-pixel loop. Sources are
 * word arrays addressed exactly like the shader's sourceBuffer[i].
 * quad_reference_execute() runs a QuadPlan the way quad_pass.c records it:
 * clears, then each draw's fragment computation followed by the blend unit
 * with round-to-nearest UNORM8 arithmetic, which for integer inputs equals
 * composite.comp's (x+127)/255 rounding. */
#include "quad_plan.h"
#include <stdint.h>

static inline uint32_t quad_ref_scale8(uint32_t x,uint32_t f){return (x*f+127u)/255u;}
static inline uint32_t quad_ref_channel(uint32_t p,unsigned c){return (p>>(8*c))&255u;}
static inline uint32_t quad_ref_pack(const uint32_t c[4]){return c[0]|(c[1]<<8)|(c[2]<<16)|(c[3]<<24);}
/* Source word sampled for destination pixel (x,y) of layer l, or 0 if the
 * pixel is outside the layer. */
static inline int quad_ref_sample(const uint32_t *l,const uint32_t *source,int32_t x,int32_t y,uint32_t *out){
    const int32_t dx=(int32_t)l[8],dy=(int32_t)l[9],cx=(int32_t)l[12],cy=(int32_t)l[13];
    const uint32_t dw=l[10],dh=l[11],cw=l[14],ch=l[15];
    if(x<dx || y<dy || x>=dx+(int32_t)dw || y>=dy+(int32_t)dh || x<cx || y<cy || x>=cx+(int32_t)cw || y>=cy+(int32_t)ch)return 0;
    const uint32_t sw=l[6],sh=l[7],u=(uint32_t)(x-dx),v=(uint32_t)(y-dy);
    const uint32_t sx=l[4]+(sw==dw?u:(2u*u+1u)*sw/(2u*dw));
    const uint32_t sy=l[5]+(sh==dh?v:(2u*v+1u)*sh/(2u*dh));
    *out=source[l[0]+sy*l[1]+sx];return 1;
}
static inline uint32_t quad_ref_blend(uint32_t mode,uint32_t src,uint32_t dst){
    uint32_t s[4],d[4],r[4];
    for(unsigned c=0;c<4;++c){s[c]=quad_ref_channel(src,c);d[c]=quad_ref_channel(dst,c);}
    const uint32_t opacity=mode&255u;
    if(mode&BROOK_VK_OPAQUE_SOURCE)s[3]=255u;
    if(opacity==255u && s[3]==255u)return quad_ref_pack(s);
    if(mode&BROOK_VK_STRAIGHT_ALPHA){
        const uint32_t a=quad_ref_scale8(s[3],opacity),inv=255u-a;
        for(unsigned c=0;c<3;++c)r[c]=(s[c]*a+d[c]*inv+127u)/255u;
        r[3]=a+quad_ref_scale8(d[3],inv);if(r[3]>255u)r[3]=255u;
    }else{
        for(unsigned c=0;c<4;++c)s[c]=quad_ref_scale8(s[c],opacity);
        const uint32_t inv=255u-s[3];
        for(unsigned c=0;c<4;++c){r[c]=s[c]+quad_ref_scale8(d[c],inv);if(r[c]>255u)r[c]=255u;}
    }
    return quad_ref_pack(r);
}
static inline uint32_t quad_reference_pixel(const uint32_t *scene,const uint32_t *const *sources,uint32_t x,uint32_t y){
    uint32_t dst=0;
    for(uint32_t i=0;i<scene[2];++i){const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;uint32_t s;
        if(quad_ref_sample(l,sources[i],(int32_t)x,(int32_t)y,&s))dst=quad_ref_blend(l[16],s,dst);}
    return dst;
}
/* The fragment shader's output value (as integers) and the blend unit. */
static inline uint32_t quad_ref_draw(uint32_t kind,uint32_t mode,uint32_t src,uint32_t dst){
    uint32_t s[4],d[4],o[4],r[4];
    for(unsigned c=0;c<4;++c){s[c]=quad_ref_channel(src,c);d[c]=quad_ref_channel(dst,c);}
    const uint32_t opacity=mode&255u;
    if(mode&BROOK_VK_OPAQUE_SOURCE)s[3]=255u;
    if(mode&BROOK_VK_STRAIGHT_ALPHA){
        const uint32_t a=quad_ref_scale8(s[3],opacity);
        for(unsigned c=0;c<3;++c)o[c]=kind==QUAD_KIND_OVER_ZERO?(s[c]*a+127u)/255u:s[c];
        o[3]=a;
    }else for(unsigned c=0;c<4;++c)o[c]=quad_ref_scale8(s[c],opacity);
    if(kind==QUAD_KIND_OVER_ZERO || kind==QUAD_KIND_REPLACE){
        for(unsigned c=0;c<4;++c)if(o[c]>255u)o[c]=255u;
        return quad_ref_pack(o);
    }
    if(kind==QUAD_KIND_STRAIGHT){
        /* color: SRC_ALPHA, ONE_MINUS_SRC_ALPHA; alpha: ONE, ONE_MINUS_SRC_ALPHA */
        for(unsigned c=0;c<3;++c)r[c]=(o[c]*o[3]+d[c]*(255u-o[3])+127u)/255u;
        r[3]=o[3]+quad_ref_scale8(d[3],255u-o[3]);
    }else for(unsigned c=0;c<4;++c)r[c]=o[c]+quad_ref_scale8(d[c],255u-o[3]);
    for(unsigned c=0;c<4;++c)if(r[c]>255u)r[c]=255u;
    return quad_ref_pack(r);
}
static inline void quad_reference_execute(const uint32_t *scene,const uint32_t *const *sources,const struct QuadPlan *plan,
                                          uint32_t *output,uint32_t stride){
    for(uint32_t i=0;i<plan->clearCount;++i){const struct QuadRect r=plan->clears[i];
        for(int32_t y=r.y0;y<r.y1;++y)for(int32_t x=r.x0;x<r.x1;++x)output[(uint64_t)y*stride+x]=0;}
    for(uint32_t i=0;i<plan->drawCount;++i){const struct QuadDraw *q=&plan->draws[i];
        const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+q->layer*BROOK_VK_LAYER_WORDS;
        for(int32_t y=q->rect.y0;y<q->rect.y1;++y)for(int32_t x=q->rect.x0;x<q->rect.x1;++x){uint32_t s=0;
            if(!quad_ref_sample(l,sources[q->layer],x,y,&s))continue; /* never happens: draws lie inside the layer */
            uint32_t *p=&output[(uint64_t)y*stride+x];*p=quad_ref_draw(q->kind,l[16],s,*p);}
    }
}

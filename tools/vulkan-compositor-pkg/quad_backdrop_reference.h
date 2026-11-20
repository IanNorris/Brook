#pragma once
#include "quad_backdrop.h"
#include "quad_reference.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Slow CPU oracle for the glass backdrop (quad_backdrop.h): each tile's
 * capture, dual-Kawase downsamples and upsamples are recomputed in double
 * precision from the unblurred layers, rounding to UNORM8 after every pass
 * as the atlas does, then the glass draw's bilinear tap and blend. GPU
 * results differ by rounding only (ties, float32, filter precision). */
struct QuadBackdropRef {
    float *final[QUAD_BACKDROP_TILES]; /* blurred mip QUAD_BACKDROP_FINAL, 4 channels per texel */
    uint32_t width[QUAD_BACKDROP_TILES],height[QUAD_BACKDROP_TILES];
};
static inline double quad_backdrop_ref_q(double v){
    if(v<0)v=0;
    if(v>1)v=1;
    return floor(v*255.0+0.5)/255.0;
}
/* Clamped bilinear tap of a w x h level at texel coordinates (x,y). */
static inline void quad_backdrop_ref_tap(const float *level,uint32_t w,uint32_t h,double x,double y,double weight,double out[4]){
    if(x<0.5)x=0.5;
    if(y<0.5)y=0.5;
    if(x>w-0.5)x=w-0.5;
    if(y>h-0.5)y=h-0.5;
    x-=0.5;y-=0.5;
    const uint32_t x0=(uint32_t)x,y0=(uint32_t)y,x1=x0+1<w?x0+1:x0,y1=y0+1<h?y0+1:y0;
    const double fx=x-x0,fy=y-y0;
    const double wts[4]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
    const size_t at[4]={(size_t)y0*w+x0,(size_t)y0*w+x1,(size_t)y1*w+x0,(size_t)y1*w+x1};
    for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)out[c]+=weight*wts[k]*level[4*at[k]+c];
}
static inline void quad_backdrop_ref_free(struct QuadBackdropRef *r){
    for(unsigned t=0;t<QUAD_BACKDROP_TILES;++t){free(r->final[t]);r->final[t]=NULL;}
}
static inline int quad_backdrop_ref_build(struct QuadBackdropRef *r,const struct QuadBackdrop *plan,const uint32_t *scene,
        const uint32_t *const *sources,const struct QuadRect *holes){
    memset(r,0,sizeof(*r));
    for(unsigned t=0;t<plan->count;++t){
        const struct QuadBackdropTile *tile=&plan->tiles[t];
        const struct QuadRect c=tile->capture;
        const int32_t block=1<<tile->base;
        uint32_t w[QUAD_BACKDROP_MIPS],h[QUAD_BACKDROP_MIPS];float *level[QUAD_BACKDROP_MIPS]={0};
        w[0]=(uint32_t)(tile->atlas.x1-tile->atlas.x0);h[0]=(uint32_t)(tile->atlas.y1-tile->atlas.y0);
        for(unsigned m=1;m<=tile->mips;++m){w[m]=(w[0]+(1u<<m)-1)>>m;h[m]=(h[0]+(1u<<m)-1)>>m;}
        for(unsigned m=0;m<=tile->mips;++m)if(!(level[m]=calloc((size_t)w[m]*h[m]*4,sizeof(float)))){
            for(unsigned k=0;k<=tile->mips;++k)free(level[k]);
            quad_backdrop_ref_free(r);return 0;
        }
        /* Capture: one layer at a time over the cleared tile. */
        for(unsigned i=quad_backdrop_bottom(scene,holes,tile);i<tile->below;++i){
            const uint32_t *l=quad_backdrop_layer(scene,i);
            const struct QuadRect rr=quad_rect_intersect(quad_layer_bounds(l,scene[0],scene[1]),c);
            if(quad_rect_empty(rr) || (holes && quad_rect_contains(holes[i],rr)))continue;
            const struct QuadRect hole=holes?quad_rect_intersect(holes[i],rr):(struct QuadRect){0};
            const int32_t tx0=(rr.x0-c.x0)/block,ty0=(rr.y0-c.y0)/block;
            const int32_t tx1=(rr.x1-c.x0+block-1)/block,ty1=(rr.y1-c.y0+block-1)/block;
            const double opacity=(l[16]&255u)/(255.0*block*block);
            for(int32_t ty=ty0;ty<ty1;++ty)for(int32_t tx=tx0;tx<tx1;++tx){
                double sum[4]={0};
                for(int32_t j=0;j<block;++j)for(int32_t i2=0;i2<block;++i2){
                    int32_t px=c.x0+tx*block+i2,py=c.y0+ty*block+j;
                    if(px>c.x1-1)px=c.x1-1;
                    if(py>c.y1-1)py=c.y1-1;
                    if(px<rr.x0 || py<rr.y0 || px>=rr.x1 || py>=rr.y1)continue;
                    if(!quad_rect_empty(hole) && px>=hole.x0 && py>=hole.y0 && px<hole.x1 && py<hole.y1)continue;
                    uint32_t word=0;
                    if(!quad_ref_sample(l,sources[i],px,py,&word))continue;
                    double v[4];for(unsigned k=0;k<4;++k)v[k]=quad_ref_channel(word,k)/255.0;
                    if(l[16]&BROOK_VK_OPAQUE_SOURCE)v[3]=1;
                    if(l[16]&BROOK_VK_STRAIGHT_ALPHA)for(unsigned k=0;k<3;++k)v[k]*=v[3];
                    for(unsigned k=0;k<4;++k)sum[k]+=v[k];
                }
                float *d=&level[0][4*((size_t)ty*w[0]+tx)];
                for(unsigned k=0;k<4;++k)sum[k]*=opacity;
                const double keep=1-sum[3];
                for(unsigned k=0;k<4;++k)d[k]=(float)quad_backdrop_ref_q(sum[k]+keep*d[k]);
            }
        }
        for(unsigned m=1;m<=tile->mips;++m)
            for(uint32_t y=0;y<h[m];++y)for(uint32_t x=0;x<w[m];++x){
                double s[4]={0};const double cx=2.0*x+1,cy=2.0*y+1;
                quad_backdrop_ref_tap(level[m-1],w[m-1],h[m-1],cx,cy,4,s);
                for(int dy=-1;dy<=1;dy+=2)for(int dx=-1;dx<=1;dx+=2)
                    quad_backdrop_ref_tap(level[m-1],w[m-1],h[m-1],cx+dx,cy+dy,1,s);
                for(unsigned k=0;k<4;++k)level[m][4*((size_t)y*w[m]+x)+k]=(float)quad_backdrop_ref_q(s[k]/8);
            }
        for(unsigned m=tile->mips;m-->QUAD_BACKDROP_FINAL;)
            for(uint32_t y=0;y<h[m];++y)for(uint32_t x=0;x<w[m];++x){
                double s[4]={0};const double ux=(x+0.5)/2,uy=(y+0.5)/2;
                const float *from=level[m+1];const uint32_t fw=w[m+1],fh=h[m+1];
                quad_backdrop_ref_tap(from,fw,fh,ux-1,uy,1,s);quad_backdrop_ref_tap(from,fw,fh,ux+1,uy,1,s);
                quad_backdrop_ref_tap(from,fw,fh,ux,uy-1,1,s);quad_backdrop_ref_tap(from,fw,fh,ux,uy+1,1,s);
                for(int dy=-1;dy<=1;dy+=2)for(int dx=-1;dx<=1;dx+=2)
                    quad_backdrop_ref_tap(from,fw,fh,ux+0.5*dx,uy+0.5*dy,2,s);
                for(unsigned k=0;k<4;++k)level[m][4*((size_t)y*w[m]+x)+k]=(float)quad_backdrop_ref_q(s[k]/12);
            }
        for(unsigned m=0;m<=tile->mips;++m)if(m!=QUAD_BACKDROP_FINAL)free(level[m]);
        r->final[t]=level[QUAD_BACKDROP_FINAL];r->width[t]=w[QUAD_BACKDROP_FINAL];r->height[t]=h[QUAD_BACKDROP_FINAL];
    }
    return 1;
}
/* The glass draw's backdrop (UNORM8 word) at desktop pixel (x,y) of tile t. */
static inline uint32_t quad_backdrop_ref_backdrop(const struct QuadBackdropRef *r,const struct QuadBackdrop *plan,
        unsigned t,int32_t x,int32_t y){
    const struct QuadBackdropTile *tile=&plan->tiles[t];
    const double scale=(double)(1<<(tile->base+QUAD_BACKDROP_FINAL));
    double s[4]={0};
    quad_backdrop_ref_tap(r->final[t],r->width[t],r->height[t],(x+0.5-tile->capture.x0)/scale,(y+0.5-tile->capture.y0)/scale,1,s);
    uint32_t c[4];for(unsigned k=0;k<4;++k)c[k]=(uint32_t)floor(s[k]*255.0+0.5);
    return quad_ref_pack(c);
}
static inline uint32_t quad_backdrop_reference_pixel(const struct QuadBackdropRef *r,const struct QuadBackdrop *plan,
        const uint32_t *scene,const uint32_t *const *sources,int32_t x,int32_t y){
    uint32_t pixel=0;
    for(unsigned i=0;i<scene[2];++i){
        const uint32_t *layer=quad_backdrop_layer(scene,i);
        uint32_t sample;
        if(!quad_ref_sample(layer,sources[i],x,y,&sample))continue;
        const uint32_t alpha=(layer[16]&BROOK_VK_OPAQUE_SOURCE)?255u:sample>>24,opacity=layer[16]&255u;
        uint32_t base=pixel;
        if((opacity!=255u || alpha!=255u) && alpha && opacity)
            for(unsigned k=0;k<plan->count;++k){
                const struct QuadRect s=plan->tiles[k].screen;
                if(plan->tiles[k].layer!=i || x<s.x0 || x>=s.x1 || y<s.y0 || y>=s.y1)continue;
                base=quad_backdrop_ref_backdrop(r,plan,k,x,y);
                break;
            }
        pixel=quad_ref_blend(layer[16],sample,base);
    }
    return pixel;
}

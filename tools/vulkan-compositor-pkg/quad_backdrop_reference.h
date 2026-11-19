#pragma once
#include "quad_backdrop.h"
#include "quad_reference.h"

/* Slow CPU oracle: independently render each tiny tile by sampling the
 * unblurred stack beneath it, then bilinearly reconstruct the backdrop. */
static inline uint32_t quad_backdrop_ref_texel(const struct QuadBackdropTile *t,const uint32_t *scene,
        const uint32_t *const *sources,unsigned x,unsigned y)
{
    const uint32_t w=t->atlas.x1-t->atlas.x0,h=t->atlas.y1-t->atlas.y0;
    const uint32_t sx=t->screen.x0+(2*x+1)*(t->screen.x1-t->screen.x0)/(2*w);
    const uint32_t sy=t->screen.y0+(2*y+1)*(t->screen.y1-t->screen.y0)/(2*h);
    uint32_t pixel=0;
    for(unsigned i=0;i<t->below;++i){
        const uint32_t *layer=quad_backdrop_layer(scene,i);
        uint32_t sample;
        if(quad_ref_sample(layer,sources[i],sx,sy,&sample))pixel=quad_ref_blend(layer[16],sample,pixel);
    }
    return pixel;
}

static inline void quad_backdrop_ref_linear(const struct QuadBackdropTile *t,const uint32_t *scene,
        const uint32_t *const *sources,double x,double y,double weight,double pixel[4])
{
    const unsigned w=t->atlas.x1-t->atlas.x0,h=t->atlas.y1-t->atlas.y0;
    if(x<0)x=0;
    if(y<0)y=0;
    if(x>w-1)x=w-1;
    if(y>h-1)y=h-1;
    const unsigned ix=(unsigned)x,iy=(unsigned)y;
    const unsigned nx=ix+1<w?ix+1:ix,ny=iy+1<h?iy+1:iy;
    const uint32_t samples[]={quad_backdrop_ref_texel(t,scene,sources,ix,iy),quad_backdrop_ref_texel(t,scene,sources,nx,iy),
                               quad_backdrop_ref_texel(t,scene,sources,ix,ny),quad_backdrop_ref_texel(t,scene,sources,nx,ny)};
    const double dx=x-ix,dy=y-iy;
    const double weights[]={(1-dx)*(1-dy),dx*(1-dy),(1-dx)*dy,dx*dy};
    for(unsigned c=0;c<4;++c)
        for(unsigned i=0;i<4;++i)pixel[c]+=quad_ref_channel(samples[i],c)*weights[i]*weight;
}

static inline uint32_t quad_backdrop_reference_pixel(const struct QuadBackdrop *plan,const uint32_t *scene,
        const uint32_t *const *sources,int32_t x,int32_t y)
{
    uint32_t pixel=0;
    for(unsigned i=0;i<scene[2];++i){
        const uint32_t *layer=quad_backdrop_layer(scene,i);
        uint32_t sample;
        if(!quad_ref_sample(layer,sources[i],x,y,&sample))continue;
        for(unsigned k=0;k<plan->count && (sample>>24) && (sample>>24)!=255;++k){
            const struct QuadBackdropTile *t=&plan->tiles[k];
            const struct QuadRect r=t->screen;
            if(t->layer!=i || x<r.x0 || x>=r.x1 || y<r.y0 || y>=r.y1)continue;
            const double sx=(double)(t->atlas.x1-t->atlas.x0)/(r.x1-r.x0);
            const double sy=(double)(t->atlas.y1-t->atlas.y0)/(r.y1-r.y0);
            const double px=(x+0.5-r.x0)*sx-0.5,py=(y+0.5-r.y0)*sy-0.5;
            double channels[4]={0};
            quad_backdrop_ref_linear(t,scene,sources,px,py,t->radius<=3?1:0.5,channels);
            if(t->radius>3){
                const double dx=t->radius*sx/2,dy=t->radius*sy/2;
                quad_backdrop_ref_linear(t,scene,sources,px+dx,py,0.125,channels);
                quad_backdrop_ref_linear(t,scene,sources,px-dx,py,0.125,channels);
                quad_backdrop_ref_linear(t,scene,sources,px,py+dy,0.125,channels);
                quad_backdrop_ref_linear(t,scene,sources,px,py-dy,0.125,channels);
            }
            uint32_t c[4];for(unsigned n=0;n<4;++n)c[n]=(uint32_t)(channels[n]+0.5);
            pixel=quad_ref_pack(c);
            break;
        }
        pixel=quad_ref_blend(layer[16],sample,pixel);
    }
    return pixel;
}

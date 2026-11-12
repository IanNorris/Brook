#include "surface.h"
#include "scene.h"
#include <stdlib.h>
#include <string.h>

enum { SURFACE_MAX_WORDS=32*1024*1024 };
struct BrookVkSurface { BrookVkRenderer *renderer,*external; size_t capacity,externalCapacity; int allowSoftware,uncertain; };

BrookVkSurface *brook_vk_surface_create(int allowSoftware) {
    BrookVkSurface *s=calloc(1,sizeof(*s));if(s)s->allowSoftware=allowSoftware;return s;
}
int brook_vk_surface_destroy(BrookVkSurface *s) {
    if(!s)return 1;
    if(s->uncertain)return 0;
    if(s->renderer && !brook_vk_renderer_destroy(s->renderer))return 0;
    s->renderer=NULL;
    if(s->external && !brook_vk_renderer_destroy(s->external))return 0;
    free(s);return 1;
}
const char *brook_vk_surface_device(const BrookVkSurface *s) {
    return s?brook_vk_renderer_device(s->external?s->external:s->renderer):NULL;
}
BrookVkResult brook_vk_surface_compose_external(BrookVkSurface *s,uint32_t width,uint32_t height,
        const struct BrookVkSurfaceExternalLayer *layers,size_t count,uint32_t *output,size_t outputWords) {
    if(!s || s->uncertain || !output || !layers || !count || count>BROOK_VK_MAX_LAYERS ||
       !width || !height || width>BROOK_VK_MAX_DIMENSION || height>BROOK_VK_MAX_DIMENSION ||
       (uint64_t)width*height>outputWords)return BROOK_VK_REJECTED;
    const size_t header=BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS;
    const uint64_t words=(uint64_t)header+(uint64_t)width*height;
    if(words>SURFACE_MAX_WORDS)return BROOK_VK_REJECTED;
    uint32_t *scene=calloc((size_t)words,4);if(!scene)return BROOK_VK_REJECTED;
    struct BrookVkExternalSource sources[BROOK_VK_MAX_LAYERS];size_t sizes[BROOK_VK_MAX_LAYERS];
    scene[0]=width;scene[1]=height;scene[2]=(uint32_t)count;scene[3]=(uint32_t)header;
    for(size_t i=0;i<count;++i){
        const struct BrookVkSurfaceExternalLayer *l=layers+i;
        if(l->fd<0 || l->syncFd < -1 || !l->words || l->opacity>255 || (l->flags&~(BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE))){free(scene);return BROOK_VK_REJECTED;}
        sources[i]=(struct BrookVkExternalSource){l->fd,l->words,l->syncFd};sizes[i]=l->words;
        const struct BrookVkSceneLayer v={l->offset,l->stride,l->imageWidth,l->imageHeight,
            l->sx,l->sy,l->sw,l->sh,l->x,l->y,l->width,l->height,l->clipX,l->clipY,l->clipWidth,l->clipHeight,l->opacity|l->flags};
        memcpy(scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,&v,sizeof(v));
    }
    if(!brook_vk_scene_valid_multi(scene,(size_t)words,sizes,count)){free(scene);return BROOK_VK_REJECTED;}
    if(words>s->externalCapacity){
        if(s->external && !brook_vk_renderer_destroy(s->external)){s->uncertain=1;free(scene);return BROOK_VK_UNCERTAIN;}
        s->external=NULL;s->externalCapacity=0;size_t capacity=16384;
        while(capacity<words && capacity<SURFACE_MAX_WORDS)capacity*=2;
        s->external=brook_vk_renderer_create_multi(capacity,s->allowSoftware);
        if(!s->external){free(scene);return BROOK_VK_REJECTED;}
        s->externalCapacity=capacity;
    }
    BrookVkResult result=brook_vk_renderer_compose_multi(s->external,scene,(size_t)words,sources,count,output,outputWords,10000000000ULL);
    free(scene);if(result==BROOK_VK_UNCERTAIN)s->uncertain=1;return result;
}
BrookVkResult brook_vk_surface_compose(BrookVkSurface *s,uint32_t width,uint32_t height,
        const struct BrookVkSurfaceLayer *layers,size_t count,uint32_t *output,size_t outputWords) {
    if(!s || s->uncertain || !output || (count && !layers) || count>BROOK_VK_MAX_LAYERS ||
       !width || !height || width>BROOK_VK_MAX_DIMENSION || height>BROOK_VK_MAX_DIMENSION ||
       (uint64_t)width*height>outputWords)return BROOK_VK_REJECTED;
    uint64_t words=BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS;
    for(size_t i=0;i<count;++i){
        const struct BrookVkSurfaceLayer *l=layers+i;
        if(!l->pixels || !l->imageWidth || !l->imageHeight ||
           l->imageWidth>BROOK_VK_MAX_DIMENSION || l->imageHeight>BROOK_VK_MAX_DIMENSION)
            return BROOK_VK_REJECTED;
        words+=(uint64_t)l->imageWidth*l->imageHeight;
        if(words>SURFACE_MAX_WORDS)return BROOK_VK_REJECTED;
    }
    uint64_t outputOffset=words;words+=(uint64_t)width*height;
    if(words>SURFACE_MAX_WORDS)return BROOK_VK_REJECTED;
    uint32_t *scene=calloc((size_t)words,4);if(!scene)return BROOK_VK_REJECTED;
    scene[0]=width;scene[1]=height;scene[2]=(uint32_t)count;scene[3]=(uint32_t)outputOffset;
    uint32_t next=BROOK_VK_SCENE_HEADER+(uint32_t)count*BROOK_VK_LAYER_WORDS;
    for(size_t i=0;i<count;++i){
        const struct BrookVkSurfaceLayer *l=layers+i;uint32_t *v=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        uint32_t descriptor[]={next,l->imageWidth,l->imageWidth,l->imageHeight,l->sx,l->sy,l->sw,l->sh,
            (uint32_t)l->x,(uint32_t)l->y,l->width,l->height,0,0,width,height,255};
        memcpy(v,descriptor,sizeof(descriptor));
        size_t pixels=(size_t)l->imageWidth*l->imageHeight;
        memcpy(scene+next,l->pixels,pixels*4);next+=(uint32_t)pixels;
    }
    if(!brook_vk_scene_valid(scene,(size_t)words)){free(scene);return BROOK_VK_REJECTED;}
    if(words>s->capacity){
        if(s->renderer && !brook_vk_renderer_destroy(s->renderer)){s->uncertain=1;free(scene);return BROOK_VK_UNCERTAIN;}
        s->renderer=NULL;s->capacity=0;
        size_t capacity=16384;
        while(capacity<words && capacity<SURFACE_MAX_WORDS)capacity*=2;
        s->renderer=brook_vk_renderer_create(capacity,s->allowSoftware);
        if(!s->renderer){free(scene);return BROOK_VK_REJECTED;}
        s->capacity=capacity;
    }
    BrookVkResult result=brook_vk_renderer_compose(s->renderer,scene,(size_t)words,output,outputWords,10000000000ULL);
    free(scene);
    if(result==BROOK_VK_UNCERTAIN)s->uncertain=1;
    return result;
}

#pragma once
#include "scene.h"
#include "desktop_damage.h"
#include <string.h>

static inline struct DesktopDamageRect desktop_tile_layer_bounds(const uint32_t *l){
    struct DesktopDamageRect r={(int32_t)l[8],(int32_t)l[9],0,0};
    r.right=r.left+l[10];r.bottom=r.top+l[11];
    const int64_t cl=(int32_t)l[12],ct=(int32_t)l[13],cr=cl+l[14],cb=ct+l[15];
    if(r.left<cl)r.left=cl;
    if(r.top<ct)r.top=ct;
    if(r.right>cr)r.right=cr;
    if(r.bottom>cb)r.bottom=cb;
    return r;
}

/* Remove layers outside damage or completely covered within every damaged
 * intersection by one later, explicitly opaque layer. Different damage
 * rectangles may have different occluders. Keep the backdrop first because
 * tile packing uses it, preserve order, and retain original source indices. */
static inline int desktop_tile_cull_policy(const uint32_t *scene,size_t words,const size_t *sources,size_t count,
        const struct DesktopDamageSet *damage,uint32_t *out,size_t capacity,uint32_t *indices,size_t *outCount,int occlusion){
    if(!scene || !sources || !damage || !out || !indices || !outCount || !count || count>BROOK_VK_MAX_LAYERS ||
       !damage->count || damage->count>BROOK_VK_MAX_TILES || capacity<BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS ||
       !brook_vk_scene_valid_multi_output(scene,words,sources,count))return 0;
    for(uint32_t i=0;i<damage->count;++i){const struct DesktopDamageRect *r=&damage->rects[i];
        if(r->left<0 || r->top<0 || r->right<=r->left || r->bottom<=r->top ||
           r->right>scene[0] || r->bottom>scene[1])return 0;
    }
    uint32_t local[BROOK_VK_SCENE_HEADER+BROOK_VK_MAX_LAYERS*BROOK_VK_LAYER_WORDS];
    uint32_t selection[BROOK_VK_MAX_LAYERS];size_t n=0;
    memcpy(local,scene,BROOK_VK_SCENE_HEADER*sizeof(*local));
    for(size_t i=0;i<count;++i){
        const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        int keep=i==0;
        if(!keep && (l[16]&255u)){
            const struct DesktopDamageRect bounds=desktop_tile_layer_bounds(l);
            for(uint32_t j=0;j<damage->count;++j){const struct DesktopDamageRect *d=&damage->rects[j];
                struct DesktopDamageRect r={bounds.left>d->left?bounds.left:d->left,bounds.top>d->top?bounds.top:d->top,
                    bounds.right<d->right?bounds.right:d->right,bounds.bottom<d->bottom?bounds.bottom:d->bottom};
                if(r.left>=r.right || r.top>=r.bottom)continue;
                int covered=0;
                for(size_t k=i+1;occlusion && k<count;++k){const uint32_t *upper=scene+BROOK_VK_SCENE_HEADER+k*BROOK_VK_LAYER_WORDS;
                    if((upper[16]&(BROOK_VK_OPAQUE_SOURCE|255u))!=(BROOK_VK_OPAQUE_SOURCE|255u))continue;
                    const struct DesktopDamageRect u=desktop_tile_layer_bounds(upper);
                    if(u.left<=r.left && u.top<=r.top && u.right>=r.right && u.bottom>=r.bottom){covered=1;break;}
                }
                if(!covered){keep=1;break;}
            }
        }
        if(keep){selection[n]=(uint32_t)i;memcpy(local+BROOK_VK_SCENE_HEADER+n*BROOK_VK_LAYER_WORDS,l,BROOK_VK_LAYER_WORDS*sizeof(*l));++n;}
    }
    local[2]=(uint32_t)n;memcpy(out,local,(BROOK_VK_SCENE_HEADER+n*BROOK_VK_LAYER_WORDS)*sizeof(*out));
    memcpy(indices,selection,n*sizeof(*indices));*outCount=n;return 1;
}
static inline int desktop_tile_cull(const uint32_t *scene,size_t words,const size_t *sources,size_t count,
        const struct DesktopDamageSet *damage,uint32_t *out,size_t capacity,uint32_t *indices,size_t *outCount){
    return desktop_tile_cull_policy(scene,words,sources,count,damage,out,capacity,indices,outCount,1);
}

/* Transform an already bounded multi-source scene into tile coordinates.
 * Source pixels, scaling and clipping remain unchanged; the separate GPU
 * output contains only this tile. Reject transformations outside the scene
 * ABI instead of truncating coordinates. This does not publish a display. */
static inline int desktop_tile_scene(const uint32_t *scene,size_t words,const size_t *sources,size_t count,
                                     uint32_t x,uint32_t y,uint32_t width,uint32_t height,
                                     uint32_t *out,size_t capacity){
    if(!scene || !out || !brook_vk_scene_valid_multi_output(scene,words,sources,count) ||
       !width || !height || x>=scene[0] || y>=scene[1] || width>scene[0]-x || height>scene[1]-y)return 0;
    const size_t metadata=BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS;
    if(capacity<metadata)return 0;
    uint32_t copy[BROOK_VK_SCENE_HEADER+BROOK_VK_MAX_LAYERS*BROOK_VK_LAYER_WORDS];
    memcpy(copy,scene,metadata*sizeof(*copy));
    for(size_t i=0;i<count;++i){
        uint32_t *l=copy+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        const int64_t dx=(int32_t)l[8]-(int64_t)x,dy=(int32_t)l[9]-(int64_t)y;
        const int64_t cx=(int32_t)l[12]-(int64_t)x,cy=(int32_t)l[13]-(int64_t)y;
        if(dx<-(int64_t)BROOK_VK_MAX_DIMENSION || dx>BROOK_VK_MAX_DIMENSION ||
           dy<-(int64_t)BROOK_VK_MAX_DIMENSION || dy>BROOK_VK_MAX_DIMENSION ||
           cx<-(int64_t)BROOK_VK_MAX_DIMENSION || cx>BROOK_VK_MAX_DIMENSION ||
           cy<-(int64_t)BROOK_VK_MAX_DIMENSION || cy>BROOK_VK_MAX_DIMENSION)return 0;
        l[8]=(uint32_t)(int32_t)dx;l[9]=(uint32_t)(int32_t)dy;
        l[12]=(uint32_t)(int32_t)cx;l[13]=(uint32_t)(int32_t)cy;
    }
    copy[0]=width;copy[1]=height;copy[3]=0;
    if(!brook_vk_scene_valid_multi_output(copy,metadata,sources,count))return 0;
    memcpy(out,copy,metadata*sizeof(*copy));return 1;
}

/* Copy the exact opaque, 1:1 full-screen backdrop into a separate tight tile
 * buffer and produce tile metadata for that replacement source. Caller must
 * keep both buffers stable and synchronize CPU writes before GPU use. */
static inline int desktop_tile_crop_backdrop(const uint32_t *scene,size_t words,const size_t *sources,size_t count,
        uint32_t x,uint32_t y,uint32_t width,uint32_t height,const uint32_t *backdrop,
        uint32_t *cropped,size_t croppedWords,uint32_t *out,size_t capacity){
    if(!scene || !sources || !count || count>BROOK_VK_MAX_LAYERS || !backdrop || !cropped || !out || words<4+BROOK_VK_LAYER_WORDS ||
       !width || !height || (uint64_t)width*height>croppedWords || sources[0]>SIZE_MAX/4 || croppedWords>SIZE_MAX/4)return 0;
    const uint32_t *l=scene+BROOK_VK_SCENE_HEADER;
    if(l[2]!=scene[0] || l[3]!=scene[1] || l[4] || l[5] || l[6]!=scene[0] || l[7]!=scene[1] ||
       l[8] || l[9] || l[10]!=scene[0] || l[11]!=scene[1] || l[12] || l[13] ||
       l[14]!=scene[0] || l[15]!=scene[1] || l[16]!=255)return 0;
    const uintptr_t src=(uintptr_t)backdrop,dst=(uintptr_t)cropped;
    const size_t srcBytes=sources[0]*4,dstBytes=(size_t)width*height*4;
    if(src>UINTPTR_MAX-srcBytes || dst>UINTPTR_MAX-dstBytes || (src<dst+dstBytes && dst<src+srcBytes))return 0;
    uint32_t copy[BROOK_VK_SCENE_HEADER+BROOK_VK_MAX_LAYERS*BROOK_VK_LAYER_WORDS];
    const size_t metadata=BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS;
    const uintptr_t meta=(uintptr_t)out,original=(uintptr_t)scene;const size_t metaBytes=metadata*4;
    if(meta>UINTPTR_MAX-metaBytes || original>UINTPTR_MAX-metaBytes ||
       (meta<dst+dstBytes && dst<meta+metaBytes) || (original<dst+dstBytes && dst<original+metaBytes) ||
       (meta<src+srcBytes && src<meta+metaBytes))return 0;
    if(capacity<metadata || !desktop_tile_scene(scene,words,sources,count,x,y,width,height,copy,metadata))return 0;
    for(uint32_t row=0;row<height;++row)
        memcpy(cropped+(size_t)row*width,backdrop+l[0]+(size_t)(y+row)*l[1]+x,(size_t)width*4);
    uint32_t *base=copy+BROOK_VK_SCENE_HEADER;
    const uint32_t replacement[]={0,width,width,height,0,0,width,height,0,0,width,height,0,0,width,height,255};
    memcpy(base,replacement,sizeof(replacement));memcpy(out,copy,metadata*sizeof(*out));return 1;
}

struct DesktopPackedTile {uint32_t x,y,width,height;uint64_t offsetBytes;};
enum { DESKTOP_TILE_ALIGNMENT_WORDS=64/sizeof(uint32_t) };
/* Prepare disjoint batch outputs with tight rows. Optionally copy cropped
 * backdrop tiles, or retain the original source and transform its coordinates.
 * Tile bases are cache-line aligned for the Broadwell linear blitter.
 * On failure the caller must discard all outputs; no GPU submission has
 * occurred. All input/output arrays must be independent. Source buffers
 * remain caller-owned and require synchronization. */
static inline int desktop_tile_pack(const uint32_t *scene,size_t words,const size_t *sources,size_t count,
        const struct DesktopDamageSet *damage,const uint32_t *backdrop,uint32_t *packed,size_t packedWords,
        uint32_t *scenes,size_t sceneWords,struct DesktopPackedTile *tiles,size_t tileCapacity){
    if(!scene || words<BROOK_VK_SCENE_HEADER || !sources || !damage || !damage->count || damage->count>BROOK_VK_MAX_TILES ||
       (!!backdrop!=!!packed) || !scenes || !tiles || tileCapacity<damage->count ||
       sceneWords<(size_t)damage->count*BROOK_VK_TILE_STRIDE)return 0;
    uint64_t total=0;
    uint32_t offsets[BROOK_VK_MAX_TILES];
    for(uint32_t i=0;i<damage->count;++i){const struct DesktopDamageRect *r=&damage->rects[i];
        if(r->left<0 || r->top<0 || r->right<=r->left || r->bottom<=r->top ||
           r->right>scene[0] || r->bottom>scene[1])return 0;
        for(uint32_t j=0;j<i;++j){const struct DesktopDamageRect *p=&damage->rects[j];
            if(r->left<p->right && p->left<r->right && r->top<p->bottom && p->top<r->bottom)return 0;}
        const uint64_t n=(uint64_t)(r->right-r->left)*(uint64_t)(r->bottom-r->top);
        total=(total+DESKTOP_TILE_ALIGNMENT_WORDS-1)&~(uint64_t)(DESKTOP_TILE_ALIGNMENT_WORDS-1);
        if(total>UINT32_MAX || n>UINT32_MAX-total || total+n>packedWords)return 0;
        offsets[i]=(uint32_t)total;
        total+=n;
    }
    for(uint32_t i=0;i<damage->count;++i){const struct DesktopDamageRect *r=&damage->rects[i];
        const uint32_t width=(uint32_t)(r->right-r->left),height=(uint32_t)(r->bottom-r->top);
        const uint32_t offset=offsets[i];
        uint32_t *out=scenes+(size_t)i*BROOK_VK_TILE_STRIDE;
        const uint32_t x=(uint32_t)r->left,y=(uint32_t)r->top;
        if(backdrop){
            if(!desktop_tile_crop_backdrop(scene,words,sources,count,x,y,width,height,
                                           backdrop,packed+offset,packedWords-offset,out,BROOK_VK_TILE_STRIDE))return 0;
            out[4]=offset;
        }else if(!desktop_tile_scene(scene,words,sources,count,x,y,width,height,out,BROOK_VK_TILE_STRIDE))return 0;
        out[3]=offset;
        tiles[i]=(struct DesktopPackedTile){(uint32_t)r->left,(uint32_t)r->top,width,height,(uint64_t)offset*4};
    }
    return 1;
}

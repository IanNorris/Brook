#pragma once
#include <stddef.h>
#include <stdint.h>

/* Portable packed scene ABI for composite.comp. Pixels are BGRA8, normally
 * premultiplied; the per-layer flag admits straight-alpha client buffers.
 * The destination belongs to the caller; the shader does not own or
 * change display scanout. Invalid scenes must never be submitted to the GPU. */
enum { BROOK_VK_SCENE_HEADER=4, BROOK_VK_LAYER_WORDS=17,
       BROOK_VK_MAX_LAYERS=64, BROOK_VK_MAX_DIMENSION=16384,
       BROOK_VK_STRAIGHT_ALPHA=256, BROOK_VK_OPAQUE_SOURCE=512, BROOK_VK_MAX_TILES=32,
       BROOK_VK_TILE_STRIDE=BROOK_VK_SCENE_HEADER+BROOK_VK_MAX_LAYERS*BROOK_VK_LAYER_WORDS };

/* Coordinates are integer pixels; scaling samples nearest at pixel centers.
 * Other sampling modes, fractional coordinates and transforms need a future
 * ABI and must not be silently approximated by this initial implementation. */
struct BrookVkSceneHeader { uint32_t width,height,layerCount,outputOffsetWords; };
struct BrookVkSceneLayer {
    uint32_t sourceOffsetWords,sourceStrideWords,sourceWidth,sourceHeight;
    uint32_t sourceX,sourceY,sourceWidthUsed,sourceHeightUsed;
    int32_t x,y;
    uint32_t width,height;
    int32_t clipX,clipY;
    uint32_t clipWidth,clipHeight,opacity; /* low 8 bits opacity, optional source flags */
};
#ifdef __cplusplus
static_assert(sizeof(BrookVkSceneHeader)==BROOK_VK_SCENE_HEADER*4);
static_assert(sizeof(BrookVkSceneLayer)==BROOK_VK_LAYER_WORDS*4);
#else
_Static_assert(sizeof(struct BrookVkSceneHeader)==BROOK_VK_SCENE_HEADER*4,"scene header layout");
_Static_assert(sizeof(struct BrookVkSceneLayer)==BROOK_VK_LAYER_WORDS*4,"scene layer layout");
#endif

static inline int brook_vk_scene_valid_impl(const uint32_t *words, size_t count, size_t sourceCount,
                                            const size_t *sourceCounts, size_t sourceCountCount, int external, int separateOutput) {
    if (!words || count < BROOK_VK_SCENE_HEADER || count > UINT32_MAX) return 0;
    uint32_t width=words[0],height=words[1],layers=words[2],out=words[3];
    if (!width || !height || width>BROOK_VK_MAX_DIMENSION || height>BROOK_VK_MAX_DIMENSION ||
        layers>BROOK_VK_MAX_LAYERS) return 0;
    if(sourceCounts && sourceCountCount!=layers)return 0;
    uint64_t header=BROOK_VK_SCENE_HEADER+(uint64_t)layers*BROOK_VK_LAYER_WORDS;
    uint64_t end=(uint64_t)out+(uint64_t)width*height;
    if (header>count || (!separateOutput && (out<header || end>count))) return 0;
    for (uint32_t i=0;i<layers;++i) {
        const uint32_t *l=words+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        uint32_t stride=l[1],sw=l[2],sh=l[3],sx=l[4],sy=l[5],w=l[6],h=l[7];
        int32_t x=(int32_t)l[8],y=(int32_t)l[9],cx=(int32_t)l[12],cy=(int32_t)l[13];
        if (!sw || !sh || sw>BROOK_VK_MAX_DIMENSION || sh>BROOK_VK_MAX_DIMENSION ||
            stride<sw || stride>BROOK_VK_MAX_DIMENSION || !w || !h || sx>sw || sy>sh ||
            w>sw-sx || h>sh-sy || !l[10] || !l[11] || l[10]>BROOK_VK_MAX_DIMENSION ||
            l[11]>BROOK_VK_MAX_DIMENSION || l[14]>BROOK_VK_MAX_DIMENSION ||
            l[15]>BROOK_VK_MAX_DIMENSION || (l[16]&~1023u) ||
            ((l[16]&BROOK_VK_OPAQUE_SOURCE) && !(l[16]&BROOK_VK_STRAIGHT_ALPHA)) ||
            x<-(int32_t)BROOK_VK_MAX_DIMENSION || x>(int32_t)BROOK_VK_MAX_DIMENSION ||
            y<-(int32_t)BROOK_VK_MAX_DIMENSION || y>(int32_t)BROOK_VK_MAX_DIMENSION ||
            cx<-(int32_t)BROOK_VK_MAX_DIMENSION || cx>(int32_t)BROOK_VK_MAX_DIMENSION ||
            cy<-(int32_t)BROOK_VK_MAX_DIMENSION || cy>(int32_t)BROOK_VK_MAX_DIMENSION) return 0;
        uint64_t start=l[0],last=start+(uint64_t)(sh-1)*stride+sw;
        if(external) {
            if(last>(sourceCounts?sourceCounts[i]:sourceCount))return 0;
        } else if(start<header || last>count || (start<end && (uint64_t)out<last))return 0;
    }
    return 1;
}
static inline int brook_vk_scene_valid(const uint32_t *words, size_t count) {
    return brook_vk_scene_valid_impl(words,count,count,NULL,0,0,0);
}
static inline int brook_vk_scene_valid_external(const uint32_t *words, size_t count, size_t sourceCount) {
    return brook_vk_scene_valid_impl(words,count,sourceCount,NULL,0,1,0);
}
/* Multi-source shader uses descriptor i for layer i. No alias assumptions are
 * made here; import and ownership validation belong to the renderer. */
static inline int brook_vk_scene_valid_multi(const uint32_t *words,size_t count,
                                              const size_t *sourceCounts,size_t sources) {
    if(!sourceCounts)return 0;
    return brook_vk_scene_valid_impl(words,count,0,sourceCounts,sources,1,0);
}
/* Separate-output shaders access only metadata in this scene buffer; output
 * bounds and input/output aliasing are checked against the imported buffers. */
static inline int brook_vk_scene_valid_multi_output(const uint32_t *words,size_t count,
                                                     const size_t *sourceCounts,size_t sources) {
    if(!sourceCounts)return 0;
    return brook_vk_scene_valid_impl(words,count,0,sourceCounts,sources,1,1);
}

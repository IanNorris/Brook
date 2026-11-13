#pragma once
#include <stddef.h>
#include <stdint.h>
#include "scene.h"

/* Push constants for the experimental two-image full-output draw. The
 * background is output-sized; foreground geometry follows scene.h. */
struct BrookVkGraphicsLayer {
    uint32_t opacity,premultiplied;
    int32_t x,y;
    uint32_t width,height;
    int32_t clipX,clipY;
    uint32_t clipWidth,clipHeight;
    uint32_t sourceX,sourceY,sourceWidth,sourceHeight;
};
_Static_assert(sizeof(struct BrookVkGraphicsLayer)==56,"graphics layer push layout");

/* Validate host-provided coordinates before sending them to the GPU. The
 * shader performs unsigned 32-bit pixel-center products, so use the same
 * dimensions and coordinate bounds as the compute compositor. */
static inline int brook_vk_graphics_layer_valid(const struct BrookVkGraphicsLayer *l,
                                                 uint32_t outputWidth,uint32_t outputHeight,
                                                 uint32_t sourceWidth,uint32_t sourceHeight)
{
    const int32_t limit=BROOK_VK_MAX_DIMENSION;
    if(!l || !outputWidth || !outputHeight || outputWidth>(uint32_t)limit || outputHeight>(uint32_t)limit ||
       !sourceWidth || !sourceHeight || sourceWidth>(uint32_t)limit || sourceHeight>(uint32_t)limit ||
       l->opacity>255u || l->premultiplied>1u ||
       !l->width || !l->height || l->width>(uint32_t)limit || l->height>(uint32_t)limit ||
       l->clipWidth>(uint32_t)limit || l->clipHeight>(uint32_t)limit ||
       l->x < -limit || l->x > limit || l->y < -limit || l->y > limit ||
       l->clipX < -limit || l->clipX > limit || l->clipY < -limit || l->clipY > limit ||
       !l->sourceWidth || !l->sourceHeight || l->sourceX>sourceWidth || l->sourceY>sourceHeight ||
       l->sourceWidth>sourceWidth-l->sourceX || l->sourceHeight>sourceHeight-l->sourceY)return 0;
    return 1;
}

/* A linear image can replace an existing word-addressed source only if
 * Vulkan's actual subresource layout exactly matches that source. This
 * checks arithmetic and sizes, not device support, memory type, dma-buf
 * identity, producer completion, or retention through GPU retirement. */
static inline int brook_vk_graphics_linear_layout_valid(uint32_t width,uint32_t height,
        uint32_t offsetWords,uint32_t strideWords,uint64_t allocationBytes,
        uint64_t vulkanOffset,uint64_t vulkanRowPitch,uint64_t vulkanRequiredBytes)
{
    if(!width || !height || width>BROOK_VK_MAX_DIMENSION || height>BROOK_VK_MAX_DIMENSION ||
       strideWords<width || strideWords>BROOK_VK_MAX_DIMENSION ||
       vulkanOffset!=(uint64_t)offsetWords*4u || vulkanRowPitch!=(uint64_t)strideWords*4u ||
       !vulkanRequiredBytes || vulkanRequiredBytes>allocationBytes)return 0;
    const uint64_t last=((uint64_t)offsetWords+(uint64_t)(height-1u)*strideWords+width)*4u;
    return last<=vulkanRequiredBytes;
}

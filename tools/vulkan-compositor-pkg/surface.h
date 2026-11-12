#pragma once
#include <stddef.h>
#include <stdint.h>
#include "renderer.h"

/* Immutable premultiplied BGRA input. Source rectangles are integer pixels. */
struct BrookVkSurfaceLayer {
    const uint32_t *pixels;
    uint32_t imageWidth,imageHeight,sx,sy,sw,sh;
    int32_t x,y;
    uint32_t width,height;
};
/* Retained linear BGRA DMA-BUF and optional sync-file. All sizes and offsets
 * are in 32-bit pixels. The caller keeps each source unchanged until COMPLETE;
 * UNCERTAIN requires retaining it through GPU recovery. */
struct BrookVkSurfaceExternalLayer {
    int fd,syncFd;size_t words;
    uint32_t offset,stride,imageWidth,imageHeight,sx,sy,sw,sh;
    int32_t x,y;
    uint32_t width,height;
    int32_t clipX,clipY;
    uint32_t clipWidth,clipHeight,opacity,flags;
};
typedef struct BrookVkSurface BrookVkSurface;
BrookVkSurface *brook_vk_surface_create(int allowSoftware);
BrookVkResult brook_vk_surface_compose(BrookVkSurface *,uint32_t width,uint32_t height,
        const struct BrookVkSurfaceLayer *,size_t count,uint32_t *output,size_t outputWords);
BrookVkResult brook_vk_surface_compose_external(BrookVkSurface *,uint32_t width,uint32_t height,
        const struct BrookVkSurfaceExternalLayer *,size_t count,uint32_t *output,size_t outputWords);
/* Returns false and retains state after uncertain GPU completion. */
int brook_vk_surface_destroy(BrookVkSurface *);
const char *brook_vk_surface_device(const BrookVkSurface *);

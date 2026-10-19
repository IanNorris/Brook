#pragma once
#include <stdint.h>

/* One-plane layouts accepted at the Wayland/kernel boundary. An implicit
 * modifier is retained for existing VirGL images; a Venus import must require
 * LINEAR and validate this same extent against its canonical resource size. */
#define BROOK_DMABUF_XRGB8888 0x34325258u
#define BROOK_DMABUF_ARGB8888 0x34325241u
#define BROOK_DMABUF_MOD_LINEAR 0ull
#define BROOK_DMABUF_MOD_IMPLICIT 0x00ffffffffffffffull
struct BrookDmabufLayout {
    uint32_t width,height,fourcc,stride,offset,flags;
    uint64_t modifier;
};
enum BrookDmabufLayoutError {
    BROOK_DMABUF_LAYOUT_OK,
    BROOK_DMABUF_LAYOUT_DIMENSIONS,
    BROOK_DMABUF_LAYOUT_FORMAT,
    BROOK_DMABUF_LAYOUT_BOUNDS
};
static inline enum BrookDmabufLayoutError BrookDmabufValidate(
    const struct BrookDmabufLayout *layout,uint64_t bytes,int allow_implicit)
{
    if (!layout || !layout->width || !layout->height ||
        layout->width>16384 || layout->height>16384) return BROOK_DMABUF_LAYOUT_DIMENSIONS;
    if ((layout->fourcc!=BROOK_DMABUF_XRGB8888 && layout->fourcc!=BROOK_DMABUF_ARGB8888) ||
        layout->flags || (layout->modifier!=BROOK_DMABUF_MOD_LINEAR &&
        !(allow_implicit && layout->modifier==BROOK_DMABUF_MOD_IMPLICIT))) return BROOK_DMABUF_LAYOUT_FORMAT;
    const uint64_t row=(uint64_t)layout->width*4;
    if ((layout->stride&3) || (layout->offset&3) || layout->stride<row) return BROOK_DMABUF_LAYOUT_BOUNDS;
    const uint64_t end=(uint64_t)layout->offset+(uint64_t)(layout->height-1)*layout->stride+row;
    return end<=bytes ? BROOK_DMABUF_LAYOUT_OK : BROOK_DMABUF_LAYOUT_BOUNDS;
}

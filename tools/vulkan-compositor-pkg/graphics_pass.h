#pragma once
#include "renderer.h"
#include "graphics_layers.h"
#include <vulkan/vulkan.h>

typedef struct BrookVkGraphicsPass BrookVkGraphicsPass;
enum { BROOK_VK_GRAPHICS_MAX_LAYERS=16 };
enum { BROOK_VK_GRAPHICS_MAX_REGIONS=32 };
struct BrookVkGraphicsRegion {uint32_t x,y,width,height;};
struct BrookVkGraphicsBuffer {
    int fd,syncFd;
    uint32_t width,height,offsetWords,strideWords;
    uint64_t allocationBytes;
};

/* Device and queue remain caller-owned. The device must have dma-buf and
 * sync-file import extensions enabled; calls using the same queue require
 * external synchronization. One instance owns at most one pending frame. */
BrookVkGraphicsPass *brook_vk_graphics_pass_create(VkPhysicalDevice,VkDevice,VkQueue,uint32_t queueFamily);
/* Borrowed handles: the renderer must outlive every pass created from them. */
int brook_vk_renderer_graphics_context(BrookVkRenderer *,VkPhysicalDevice *,VkDevice *,VkQueue *,uint32_t *);
/* bg must cover the whole output and be opaque. Output is a distinct,
 * caller-reserved OS buffer. Every FD, producer lease and output ticket must
 * remain valid and immutable until the caller observes GPU completion and
 * completes the OS handoff. UNCERTAIN requires retention through recovery. */
BrookVkResult brook_vk_graphics_pass_submit(BrookVkGraphicsPass *,
        const struct BrookVkGraphicsBuffer *bg,const struct BrookVkGraphicsBuffer *fg,
        const struct BrookVkGraphicsBuffer *output,const struct BrookVkGraphicsLayer *layer);
/* Scene is the packed scene.h ABI with one distinct imported image per layer.
 * Output offset must be zero. More than 16 layers or an unsupported layout is
 * rejected so the caller can use its existing compute fallback. */
BrookVkResult brook_vk_graphics_pass_submit_scene(BrookVkGraphicsPass *,const uint32_t *scene,size_t words,
        const struct BrookVkGraphicsBuffer *sources,size_t count,const struct BrookVkGraphicsBuffer *output);
/* With nonzero regionCount, only these rectangles are defined in output;
 * caller may present only those rectangles and must preserve prior scanout
 * elsewhere. Zero regions means a full output frame. */
BrookVkResult brook_vk_graphics_pass_submit_regions(BrookVkGraphicsPass *,const uint32_t *scene,size_t words,
        const struct BrookVkGraphicsBuffer *sources,size_t count,const struct BrookVkGraphicsBuffer *output,
        const struct BrookVkGraphicsRegion *regions,size_t regionCount);
BrookVkResult brook_vk_graphics_pass_poll(BrookVkGraphicsPass *);
int brook_vk_graphics_pass_gpu_timestamp_ns(const BrookVkGraphicsPass *,uint64_t *);
/* Refuses pending or uncertain work. Caller controls device lifetime. */
int brook_vk_graphics_pass_destroy(BrookVkGraphicsPass *);

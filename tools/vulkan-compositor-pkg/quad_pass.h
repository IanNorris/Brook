#pragma once
/* Per-layer quad compositor: draws a QuadPlan (quad_plan.h) into a linear
 * output, one pixel-aligned rectangle per draw, with push-constant layer
 * parameters and the hardware sampler for integer upscales.
 *
 * Blending is exact (bit-identical to composite.comp): translucent layers
 * read the destination as an input attachment, with a by-region barrier
 * between layers; opaque and base layers write without reading. The Intel
 * UNORM8 blend unit rounds differently on some translucent pixels, so
 * fixed-function blending is only an opt-in (BROOK_RENDER_QUAD_FIXED_BLEND).
 *
 * Imports of renderer-owned buffers (cache=1) persist across submissions:
 * the image, view, descriptor set and, for the output, the framebuffer are
 * keyed by the buffer's dev/inode, size and layout and pinned with a
 * duplicate fd. Entries unused for a few submissions are dropped. Other
 * sources (producer-offered buffers) are imported for one submission.
 *
 * Device and queue remain caller-owned; the device needs dma-buf import and,
 * for producer fences, sync-file semaphore import. One instance owns at
 * most one pending frame. Every source and the output must stay valid and
 * unchanged until poll() returns COMPLETE; on UNCERTAIN retain everything
 * through recovery. */
#include "renderer.h"
#include "graphics_pass.h"
#include "quad_plan.h"
#include <vulkan/vulkan.h>

typedef struct BrookVkQuadPass BrookVkQuadPass;
struct BrookVkQuadSource { struct BrookVkGraphicsBuffer buffer; int cache; };
struct BrookVkQuadStats { uint64_t imports,cacheHits,evictions,submits; };

BrookVkQuadPass *brook_vk_quad_pass_create(VkPhysicalDevice,VkDevice,VkQueue,uint32_t queueFamily);
/* scene: header and layer words; sources: one per scene layer (only layers
 * the plan draws are imported; others may have fd -1). The output must be
 * distinct from every source, have offset 0 and the scene's size. Rejects
 * (without side effects) anything it cannot draw exactly. */
BrookVkResult brook_vk_quad_pass_submit(BrookVkQuadPass *,const uint32_t *scene,size_t words,
        const struct BrookVkQuadSource *sources,size_t count,const struct BrookVkQuadSource *output,
        const struct QuadPlan *plan);
BrookVkResult brook_vk_quad_pass_poll(BrookVkQuadPass *);
/* Blocks up to timeoutNs; returns PENDING on timeout. */
BrookVkResult brook_vk_quad_pass_wait(BrookVkQuadPass *,uint64_t timeoutNs);
int brook_vk_quad_pass_gpu_timestamp_ns(const BrookVkQuadPass *,uint64_t *);
struct BrookVkQuadStats brook_vk_quad_pass_stats(const BrookVkQuadPass *);
/* Refuses pending or uncertain work. */
int brook_vk_quad_pass_destroy(BrookVkQuadPass *);

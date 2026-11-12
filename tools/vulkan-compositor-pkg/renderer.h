#pragma once
#include <stddef.h>
#include <stdint.h>

/* The caller keeps scene/output stable during this synchronous call. A
 * quarantined renderer owns potentially in-flight GPU resources and must not
 * be reused or destroyed; its process supervisor handles device recovery. */
typedef struct BrookVkRenderer BrookVkRenderer;
typedef enum { BROOK_VK_UNCERTAIN=-1, BROOK_VK_REJECTED=0, BROOK_VK_COMPLETE=1, BROOK_VK_PENDING=2 } BrookVkResult;
enum { BROOK_VK_FAILURE_NONE=0, BROOK_VK_FAILURE_SUBMIT=1, BROOK_VK_FAILURE_WAIT=2 };
/* Raw VkResult values, captured without probing or changing resource state. */
struct BrookVkFailure { int stage, result, fence_status, fence_status_valid; };
struct BrookVkFailure brook_vk_renderer_failure(const BrookVkRenderer *);
/* Last completed asynchronous submission's GPU timestamp delta, if available. */
int brook_vk_renderer_gpu_timestamp_ns(const BrookVkRenderer *,uint64_t *);
/* Drain optional per-renderer timing totals; does not submit or wait. */
struct BrookVkTiming { uint64_t samples,import_ns,compose_ns,cleanup_ns,cache_hits,cache_inserts,cache_evictions; };
struct BrookVkTiming brook_vk_renderer_take_timing(BrookVkRenderer *);
struct BrookVkExternalSource { int fd; size_t words; int syncFd; };
/* Each layer has one distinct linear DMA-BUF with stable fstat dev/inode;
 * aliases and descriptors without a stable identity are rejected. syncFd=-1 means its producer
 * was already waited for. All sources must remain unchanged until completion;
 * on UNCERTAIN retain them and the renderer until device recovery. */
BrookVkRenderer *brook_vk_renderer_create_multi(size_t maxWords,int allowSoftware);
BrookVkResult brook_vk_renderer_compose_multi(BrookVkRenderer *,const uint32_t *scene,size_t words,
        const struct BrookVkExternalSource *sources,size_t sourceCount,uint32_t *output,size_t outputWords,uint64_t timeoutNs);
/* Destination is a separate linear DMA-BUF, already available for GPU writes.
 * The scene need contain only the header and layer metadata; its output
 * offset selects a word offset in the separate destination.
 * Successful return means the GPU finished; caller retains all descriptors.
 * UNCERTAIN requires retaining every source and destination through recovery. */
BrookVkRenderer *brook_vk_renderer_create_multi_output(size_t maxWords,int allowSoftware);
BrookVkResult brook_vk_renderer_compose_multi_output(BrookVkRenderer *,const uint32_t *scene,size_t words,
        const struct BrookVkExternalSource *sources,size_t sourceCount,int outputFd,size_t outputWords,uint64_t timeoutNs);
/* Batch scenes occupy BROOK_VK_TILE_STRIDE words apiece and share the same
 * source descriptors. Their output ranges must be disjoint. All dispatches
 * are submitted together and the one completion fence covers every tile. */
BrookVkRenderer *brook_vk_renderer_create_batch_output(uint32_t maxTiles,int allowSoftware);
BrookVkResult brook_vk_renderer_compose_batch_output(BrookVkRenderer *,const uint32_t *scenes,size_t words,uint32_t tiles,
        const struct BrookVkExternalSource *sources,size_t sourceCount,int outputFd,size_t outputWords,uint64_t timeoutNs);
/* Nonblocking submission for independent frame contexts. PENDING retains all
 * imports, fences and command state; poll until COMPLETE before reuse. On
 * UNCERTAIN retain the context and all source/output ownership as usual. */
BrookVkResult brook_vk_renderer_submit_batch_output(BrookVkRenderer *,const uint32_t *scenes,size_t words,uint32_t tiles,
        const struct BrookVkExternalSource *sources,size_t sourceCount,int outputFd,size_t outputWords);
BrookVkResult brook_vk_renderer_poll(BrookVkRenderer *);
BrookVkRenderer *brook_vk_renderer_create(size_t maxWords, int allowSoftware);
/* All source layers refer to one immutable linear DMA-BUF. The caller must
 * wait for producer completion before calling and retain the buffer unchanged
 * through return. A rejected call leaves output unchanged; on UNCERTAIN the
 * renderer and source must remain alive until device recovery. */
BrookVkRenderer *brook_vk_renderer_create_external(size_t maxWords, int allowSoftware);
BrookVkResult brook_vk_renderer_compose_external(BrookVkRenderer *,const uint32_t *scene,size_t words,
        int dmaBufFd,size_t sourceWords,uint32_t *output,size_t outputWords,uint64_t timeoutNs);
/* Fence variant imports a duplicate of producerSyncFd as a Vulkan sync-file;
 * the caller retains both fds. Source must not be changed or released until
 * the call completes. On UNCERTAIN it must be retained through recovery. */
BrookVkResult brook_vk_renderer_compose_external_sync(BrookVkRenderer *,const uint32_t *scene,size_t words,
        int dmaBufFd,size_t sourceWords,int producerSyncFd,uint32_t *output,size_t outputWords,uint64_t timeoutNs);
BrookVkResult brook_vk_renderer_compose(BrookVkRenderer *,const uint32_t *scene,size_t words,
                                        uint32_t *output,size_t outputWords,uint64_t timeoutNs);
int brook_vk_renderer_destroy(BrookVkRenderer *);
/* After a VK_TIMEOUT only, destroy the renderer once its own Vulkan fence is
 * confirmed signaled. Returns 0 and retains everything for all other states. */
int brook_vk_renderer_retire_timeout(BrookVkRenderer **);
const char *brook_vk_renderer_device(const BrookVkRenderer *);

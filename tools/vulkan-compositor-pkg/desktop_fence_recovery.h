#pragma once
/* The caller retains every resource while this step is pending or retained.
 * A successful Vulkan retirement may destroy the renderer, so a failed ticket
 * release is terminal for this context and must never re-run retirement. */
enum DesktopFenceRecovery { DESKTOP_FENCE_PENDING, DESKTOP_FENCE_RECOVERED, DESKTOP_FENCE_RETAIN };
struct DesktopFenceRecoveryOps {
    void *context;
    long (*observe)(void *);
    int (*retire)(void *);
    long (*release)(void *);
};
static inline enum DesktopFenceRecovery desktop_fence_recovery_step(const struct DesktopFenceRecoveryOps *ops) {
    if(!ops || !ops->observe || !ops->retire || !ops->release)return DESKTOP_FENCE_RETAIN;
    if(ops->observe(ops->context)!=1)return DESKTOP_FENCE_PENDING;
    if(!ops->retire(ops->context))return DESKTOP_FENCE_PENDING;
    return ops->release(ops->context)==0?DESKTOP_FENCE_RECOVERED:DESKTOP_FENCE_RETAIN;
}

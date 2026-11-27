// Zero-copy step 2: per-window NVK client layers (see nv_layer.h).
#include "nv_layer.h"
#include "spinlock.h"
#include "window.h"
#include "nv_disp_live.h"
#include "memory/mapping_ranges.h"

namespace brook {
namespace {
SpinLock g_layerLock;
NvLayer g_layers[WM_MAX_WINDOWS];
void (*volatile g_retireLocked)() = nullptr;
NvLayerStats g_stats{};
// No nv-disp copy may read the old layer after this returns: a kick made under the nv-aux
// lock before the swap is finished here; any later one looks the new layer up.
void Retire(MappingBacking* old) {
    if (!old) return;
#if defined(BROOK_NV_DISPLAY) && !defined(BROOK_HOST_TEST)
    if (auto* hook = __atomic_load_n(&g_retireLocked, __ATOMIC_ACQUIRE)) {
        SpinLock* aux = NvAuxSharedLock();
        SpinLockAcquire(aux);
        hook();
        SpinLockRelease(aux);
        __atomic_add_fetch(&g_stats.retires, 1, __ATOMIC_RELAXED);
    }
#endif
    MappingBackingUnref(old);
}
}

uint64_t NvLayerSet(uint32_t slot, uint64_t identity, NvLayer next) {
    if (slot >= WM_MAX_WINDOWS) { if (next.life) MappingBackingUnref(next.life); return 0; }
    if (!next.phys) { if (next.life) MappingBackingUnref(next.life); next = NvLayer{}; }
    SpinLockAcquire(&g_layerLock);
    NvLayer& l = g_layers[slot];
    MappingBacking* old = l.life;
    const uint64_t seq = l.seq + 1;
    if (next.phys) { next.identity = identity; next.seq = seq; l = next; }
    else { l = NvLayer{}; l.seq = seq; }
    SpinLockRelease(&g_layerLock);
    __atomic_add_fetch(next.phys ? &g_stats.sets : &g_stats.detaches, 1, __ATOMIC_RELAXED);
    Retire(old);
    return seq;
}
void NvLayerDetach(uint32_t slot, uint64_t identity) {
    if (slot >= WM_MAX_WINDOWS) return;
    SpinLockAcquire(&g_layerLock);
    NvLayer& l = g_layers[slot];
    MappingBacking* old = nullptr;
    if (l.phys && (!identity || l.identity == identity)) { old = l.life; const uint64_t seq = l.seq + 1; l = NvLayer{}; l.seq = seq; }
    SpinLockRelease(&g_layerLock);
    if (old) { __atomic_add_fetch(&g_stats.detaches, 1, __ATOMIC_RELAXED); Retire(old); }
}
bool NvLayerGet(uint32_t slot, uint64_t identity, NvLayer* out, bool ref) {
    if (slot >= WM_MAX_WINDOWS || !out) return false;
    SpinLockAcquire(&g_layerLock);
    const NvLayer& l = g_layers[slot];
    const bool ok = l.phys && (!identity || l.identity == identity);
    if (ok) { *out = l; if (ref && l.life) MappingBackingRef(l.life); }
    SpinLockRelease(&g_layerLock);
    if (!ok) *out = NvLayer{};
    if (!ref) out->life = nullptr;
    return ok;
}
void NvLayerPut(const NvLayer& l) { if (l.life) MappingBackingUnref(l.life); }
void NvLayerSetRetireHook(void (*retireLocked)()) { __atomic_store_n(&g_retireLocked, retireLocked, __ATOMIC_RELEASE); }
NvLayerStats NvLayerStatsGet() { return g_stats; }
}

#pragma once
// GPU-neutral fences and sync objects (M5.0). The fence implementation lives in
// intel_bdw_fence.h for history; it has no Intel hardware dependency: a backend creates a
// fence, hands it to queued work and completes it from its own retire path, which wakes
// every waiter through the shared Retire/Work event generations. These names are what a
// second backend (nouveau) uses; the i915 path keeps its Bdw* spellings.
#include <stdint.h>
#include "intel_bdw_fence.h"
#include "sync/kmutex.h"

namespace brook {
using GpuFence = BdwFence;
using GpuBinarySync = BdwBinarySync;
using GpuTimelineSync = BdwTimelineSync;
inline GpuFence* GpuFenceCreate(bool completed = false) { return BdwFenceCreate(completed); }
inline void GpuFenceRef(GpuFence* f) { BdwFenceRef(f); }
inline void GpuFenceUnref(GpuFence* f) { BdwFenceUnref(f); }
inline uint32_t GpuFenceState(const GpuFence* f, uint64_t* ts = nullptr) { return BdwFenceState(f, ts); }   // 0 pending, 1 done, 2 failed
inline bool GpuFenceComplete(GpuFence* f, bool success, uint64_t ns = 0) { return BdwFenceComplete(f, success, ns); }

// A DRM sync object: one timeline (point 0 = the binary payload).
struct BdwGpuSync;
using GpuSync = BdwGpuSync;
GpuSync* GpuSyncCreate(bool signaled);
void GpuSyncRef(GpuSync* sync);
void GpuSyncUnref(GpuSync* sync);
GpuFence* GpuSyncAcquire(GpuSync* sync);
bool GpuSyncReplace(GpuSync* sync, GpuFence* fence);
GpuFence* GpuSyncPrepareTimeline(GpuSync* sync, uint64_t point, GpuFence* dependency = nullptr);
bool GpuSyncPublishTimeline(GpuSync* sync, GpuFence* fence);
GpuFence* GpuSyncFindTimeline(GpuSync* sync, uint64_t point);
uint64_t GpuSyncQueryTimeline(GpuSync* sync, bool lastSubmitted);

// Per-open handle table of sync objects (ids from 1, never reused while the table lives).
template <uint32_t N> class GpuSyncTable {
    struct Slot { GpuSync* sync; uint32_t id; };
    KMutex lock_{};
    Slot slots_[N]{};
    uint32_t next_ = 1;
    struct Guard { KMutex* m; explicit Guard(KMutex* x) : m(x) { KMutexLock(m); } ~Guard() { KMutexUnlock(m); } };
public:
    void Init() { KMutexInit(&lock_); for (auto& s : slots_) s = {}; next_ = 1; }
    uint32_t Create(bool signaled) {
        GpuSync* s = GpuSyncCreate(signaled);
        if (!s) return 0;
        const uint32_t id = Import(s);
        GpuSyncUnref(s);
        return id;
    }
    // Returns a reference the caller drops with GpuSyncUnref.
    GpuSync* Get(uint32_t id) {
        Guard g(&lock_);
        for (auto& s : slots_) if (s.id == id && s.sync) { GpuSyncRef(s.sync); return s.sync; }
        return nullptr;
    }
    uint32_t Import(GpuSync* sync) {
        if (!sync) return 0;
        Guard g(&lock_);
        Slot* empty = nullptr;
        for (auto& s : slots_) if (!s.sync) { empty = &s; break; }
        if (!empty || !next_) return 0;
        const uint32_t id = next_++;
        GpuSyncRef(sync); *empty = {sync, id};
        return id;
    }
    bool Destroy(uint32_t id) {
        if (!id) return false;
        GpuSync* s = nullptr;
        { Guard g(&lock_); for (auto& slot : slots_) if (slot.id == id && slot.sync) { s = slot.sync; slot = {}; break; } }
        if (!s) return false;
        GpuSyncUnref(s);
        return true;
    }
    void CloseAll() { for (auto& s : slots_) if (s.sync) { GpuSyncUnref(s.sync); s = {}; } }
};
}  // namespace brook

#pragma once
#include <stdint.h>

namespace brook {

// Bounded cache of free fixed-size objects, with no allocation or destruction.
// The caller allocates on a miss and frees when Release cannot retain an object.
// Active/sleeping users do not consume cache slots. Finish all object cleanup
// before Release; callers must own each pointer exclusively and release it once.
template <typename T, unsigned N>
class ObjectCache {
    static_assert(N > 0, "cache must have storage");
    T* free_[N]{};
    uint64_t hits_ = 0, misses_ = 0, retained_ = 0, full_ = 0;
public:
    struct Stats { uint64_t hits, misses, retained, full; };
    ObjectCache() = default;
    ObjectCache(const ObjectCache&) = delete;
    ObjectCache& operator=(const ObjectCache&) = delete;
    T* TryAcquire() {
        for (unsigned i = 0; i < N; ++i) {
            if (!__atomic_load_n(&free_[i], __ATOMIC_RELAXED)) continue;
            if (T* object = __atomic_exchange_n(&free_[i], nullptr, __ATOMIC_ACQUIRE)) {
                __atomic_fetch_add(&hits_, 1, __ATOMIC_RELAXED);
                return object;
            }
        }
        __atomic_fetch_add(&misses_, 1, __ATOMIC_RELAXED);
        return nullptr;
    }
    bool Release(T* object) {
        if (!object) return false;
        for (unsigned i = 0; i < N; ++i) {
            T* empty = nullptr;
            if (__atomic_compare_exchange_n(&free_[i], &empty, object, false,
                                            __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                __atomic_fetch_add(&retained_, 1, __ATOMIC_RELAXED);
                return true;
            }
        }
        __atomic_fetch_add(&full_, 1, __ATOMIC_RELAXED);
        return false;
    }
    // Rolling counters, not an atomic snapshot of concurrent operations.
    Stats Snapshot() const {
        return {__atomic_load_n(&hits_, __ATOMIC_RELAXED),
                __atomic_load_n(&misses_, __ATOMIC_RELAXED),
                __atomic_load_n(&retained_, __ATOMIC_RELAXED),
                __atomic_load_n(&full_, __ATOMIC_RELAXED)};
    }
};

} // namespace brook

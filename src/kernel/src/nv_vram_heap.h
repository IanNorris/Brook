#pragma once
// M5.2: client VRAM allocator for the nouveau node (as nouveau on GSP-RM, Brook owns the
// client part of VRAM; RM only manages its own reserved regions). First-fit over a sorted,
// coalescing free list of [base, end) extents; 64 KiB granules; power-of-two alignment up to
// 2 MiB (big pages). Fixed capacity, no allocation inside; the caller serialises.
#include <stdint.h>

namespace brook::nvdrm {

class VramHeap {
public:
    static constexpr uint64_t kGranule = 64ull << 10;
    static constexpr uint32_t kMaxExtents = 1024;

    // [lo, hi) are rounded inwards to granules.
    void Init(uint64_t lo, uint64_t hi) {
        lo_ = (lo + kGranule - 1) & ~(kGranule - 1); hi_ = hi & ~(kGranule - 1);
        n_ = 0; used_ = 0;
        if (hi_ > lo_) { ext_[0] = {lo_, hi_}; n_ = 1; }
    }
    uint64_t Lo() const { return lo_; }
    uint64_t Hi() const { return hi_; }
    uint64_t Used() const { return used_; }
    uint64_t Free() const { return (hi_ - lo_) - used_; }
    uint32_t Extents() const { return n_; }

    // Returns false when no extent fits (or the free list would overflow).
    bool Alloc(uint64_t bytes, uint64_t align, uint64_t* out) {
        if (!bytes || !out) return false;
        bytes = (bytes + kGranule - 1) & ~(kGranule - 1);
        if (align < kGranule) align = kGranule;
        if (align & (align - 1)) return false;
        for (uint32_t i = 0; i < n_; ++i) {
            const uint64_t a = (ext_[i].base + align - 1) & ~(align - 1);
            if (a < ext_[i].base || a + bytes > ext_[i].end || a + bytes < a) continue;
            const Extent e = ext_[i];
            const bool head = a > e.base, tail = a + bytes < e.end;
            if (head && tail) {   // split in three: keep head here, insert tail after
                if (n_ >= kMaxExtents) return false;
                for (uint32_t j = n_; j > i + 1; --j) ext_[j] = ext_[j - 1];
                ext_[i] = {e.base, a}; ext_[i + 1] = {a + bytes, e.end}; ++n_;
            } else if (head) ext_[i].end = a;
            else if (tail) ext_[i].base = a + bytes;
            else { for (uint32_t j = i; j + 1 < n_; ++j) ext_[j] = ext_[j + 1]; --n_; }
            used_ += bytes; *out = a;
            return true;
        }
        return false;
    }

    // Frees [base, base+bytes). The heap keeps no allocation records (the BO owns base and
    // size); it refuses only ranges that overlap free space (a double free).
    bool Release(uint64_t base, uint64_t bytes) {
        bytes = (bytes + kGranule - 1) & ~(kGranule - 1);
        const uint64_t end = base + bytes;
        if (!bytes || (base & (kGranule - 1)) || base < lo_ || end > hi_ || end < base) return false;
        uint32_t i = 0;
        while (i < n_ && ext_[i].base < base) ++i;
        if (i > 0 && ext_[i - 1].end > base) return false;
        if (i < n_ && ext_[i].base < end) return false;
        const bool left = i > 0 && ext_[i - 1].end == base, right = i < n_ && ext_[i].base == end;
        if (left && right) { ext_[i - 1].end = ext_[i].end; for (uint32_t j = i; j + 1 < n_; ++j) ext_[j] = ext_[j + 1]; --n_; }
        else if (left) ext_[i - 1].end = end;
        else if (right) ext_[i].base = base;
        else {
            if (n_ >= kMaxExtents) return false;
            for (uint32_t j = n_; j > i; --j) ext_[j] = ext_[j - 1];
            ext_[i] = {base, end}; ++n_;
        }
        used_ -= bytes;
        return true;
    }

private:
    struct Extent { uint64_t base, end; };
    uint64_t lo_ = 0, hi_ = 0, used_ = 0;
    uint32_t n_ = 0;
    Extent ext_[kMaxExtents] = {};
};

}  // namespace brook::nvdrm

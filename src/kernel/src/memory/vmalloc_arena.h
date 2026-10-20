#pragma once
#include <stdint.h>

namespace brook {
// Caller serializes all methods. No heap allocation and no allocation-count
// limit: metadata is proportional to the size of the virtual arena. Retired
// pages remain occupied until the single drainer completes an all-CPU barrier.
template<uint64_t Pages> class VmallocArena {
    static_assert(Pages > 0);
    static constexpr uint64_t Words = (Pages + 63) / 64;
    static constexpr uint64_t SummaryWords = (Words + 63) / 64;
    enum : uint8_t { Free, Body, First, Last, Single, GuardFirst, GuardLast,
                     RetiredA, RetiredB };
    uint8_t states[(Pages + 1) / 2]{};
    uint64_t occupied[Words]{};
    uint64_t retired[2][SummaryWords]{};
    uint64_t hint = 0;
    unsigned incoming = 0, batch = 0;
    bool pending = false, draining = false;
    uint64_t reservedPages = 0, retiredPages = 0, peakPages = 0;
    uint64_t totalReservedPages = 0, totalReclaimedPages = 0, failures = 0;
    uint8_t State(uint64_t page) const {
        return (states[page / 2] >> ((page % 2) * 4)) & 15;
    }
    void Set(uint64_t page, uint8_t state) {
        const unsigned shift = (page % 2) * 4;
        states[page / 2] = (states[page / 2] & ~(15u << shift)) | (state << shift);
    }
    bool Find(uint64_t begin, uint64_t end, uint64_t count, uint64_t& first) const {
        uint64_t run = 0;
        for (uint64_t page = begin; page < end;) {
            const uint64_t word = occupied[page / 64];
            if ((page % 64) == 0 && word == ~0ULL && end - page >= 64) {
                run = 0; page += 64; continue;
            }
            if (word & (1ULL << (page % 64))) run = 0;
            else if (++run == count) { first = page + 1 - count; return true; }
            ++page;
        }
        return false;
    }
public:
    struct Range { uint64_t first, pages; };
    struct Statistics {
        uint64_t reserved, retired, peak, totalReserved, totalReclaimed, failed;
    };
    Statistics Stats() const {
        return {reservedPages, retiredPages, peakPages, totalReservedPages, totalReclaimedPages, failures};
    }
    bool Reserve(uint64_t usablePages, bool guards, uint64_t& usable) {
        const uint64_t extra = guards ? 2 : 0;
        if (!usablePages || extra > Pages || usablePages > Pages - extra) {++failures; return false;}
        const uint64_t total = usablePages + extra;
        uint64_t first;
        // The second search includes the old hint so an extent straddling that
        // hint cannot be missed. An allocation never wraps the arena boundary.
        if (!Find(hint, Pages, total, first) && !Find(0, Pages, total, first)) {++failures; return false;}
        for (uint64_t i = 0; i < total; ++i) {
            Set(first + i, Body);
            occupied[(first + i) / 64] |= 1ULL << ((first + i) % 64);
        }
        Set(first, guards ? GuardFirst : total == 1 ? Single : First);
        if (total > 1) Set(first + total - 1, guards ? GuardLast : Last);
        usable = first + (guards ? 1 : 0);
        hint = first + total == Pages ? 0 : first + total;
        reservedPages += total; totalReservedPages += total;
        if (reservedPages > peakPages) peakPages = reservedPages;
        return true;
    }
    bool Describe(uint64_t usable, uint64_t count, Range& range) const {
        if (!count || usable >= Pages || count > Pages - usable) return false;
        const uint8_t start = State(usable);
        const bool guards = start == Body && usable && State(usable - 1) == GuardFirst;
        if (guards) {
            if (count == Pages - usable || State(usable + count) != GuardLast) return false;
        } else if (start != (count == 1 ? Single : First) ||
                   (count > 1 && State(usable + count - 1) != Last)) return false;
        for (uint64_t i = guards ? 0 : 1; i < count - (guards ? 0 : 1); ++i)
            if (State(usable + i) != Body) return false;
        range = {usable - (guards ? 1 : 0), count + (guards ? 2 : 0)};
        return true;
    }
    bool Retire(uint64_t usable, uint64_t count) {
        Range range;
        if (!Describe(usable, count, range)) return false;
        for (uint64_t p = range.first; p < range.first + range.pages; ++p) {
            Set(p, incoming ? RetiredB : RetiredA);
            const uint64_t group = p / 64;
            retired[incoming][group / 64] |= 1ULL << (group % 64);
        }
        pending = true;
        retiredPages += range.pages;
        return true;
    }
    // Snapshot before the barrier. Frees during it enter the other generation.
    bool BeginReclaim() {
        if (draining || !pending) return false;
        batch = incoming; incoming ^= 1; pending = false; draining = true;
        return true;
    }
    bool CompleteReclaim() {
        if (!draining) return false;
        const uint8_t expected = batch ? RetiredB : RetiredA;
        for (uint64_t i = 0; i < SummaryWords; ++i) {
            uint64_t groups = retired[batch][i];
            while (groups) {
                const unsigned bit = __builtin_ctzll(groups);
                const uint64_t begin = (i * 64 + bit) * 64;
                for (uint64_t p = begin; p < Pages && p < begin + 64; ++p)
                    if (State(p) == expected) {
                        Set(p, Free); occupied[p / 64] &= ~(1ULL << (p % 64));
                        --reservedPages; --retiredPages; ++totalReclaimedPages;
                        if (p < hint) hint = p;
                    }
                groups &= groups - 1;
            }
            retired[batch][i] = 0;
        }
        draining = false;
        return true;
    }
};
}

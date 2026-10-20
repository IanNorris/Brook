#include "../../kernel/src/memory/vmalloc_arena.h"
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using brook::VmallocArena;

template<uint64_t Pages> static void ExhaustionAndGuards() {
    VmallocArena<Pages> arena;
    uint64_t first = 999, next = 999;
    assert(!arena.Reserve(0, false, first));
    assert(!arena.Reserve(UINT64_MAX, true, first));
    assert(!arena.BeginReclaim());
    assert(!arena.CompleteReclaim());
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        assert(arena.Reserve(Pages, false, first) && first == 0);
        assert(!arena.Reserve(1, false, next));
        if (Pages > 1) assert(!arena.Retire(first, Pages - 1));
        assert(arena.Retire(first, Pages));
        assert(!arena.Retire(first, Pages));
        assert(!arena.Reserve(1, false, next));
        assert(arena.BeginReclaim());
        assert(!arena.BeginReclaim());
        assert(!arena.Reserve(1, false, next));
        assert(arena.CompleteReclaim());
        if constexpr (Pages >= 3) {
            assert(arena.Reserve(Pages - 2, true, first) && first == 1);
            typename VmallocArena<Pages>::Range range;
            assert(arena.Describe(first, Pages - 2, range));
            assert(range.first == 0 && range.pages == Pages);
            assert(!arena.Retire(0, Pages));
            assert(!arena.Retire(first, Pages - 1));
            assert(arena.Retire(first, Pages - 2));
            assert(arena.BeginReclaim());
            assert(arena.CompleteReclaim());
        }
    }
}

static void BarrierGenerationAndCoalescing() {
    VmallocArena<8> arena;
    uint64_t a, b, c, d;
    assert(arena.Reserve(2, false, a));
    assert(arena.Reserve(2, false, b));
    assert(arena.Reserve(4, false, c));
    assert(arena.Retire(a, 2));
    assert(arena.BeginReclaim());
    // Frees concurrent with an outstanding barrier must wait for the next one,
    // including frees in the very same 64-page summary group.
    assert(arena.Retire(b, 2));
    assert(arena.CompleteReclaim());
    assert(!arena.Reserve(3, false, d));
    assert(arena.Reserve(2, false, d) && d == a);
    assert(arena.Retire(d, 2));
    assert(arena.BeginReclaim());
    assert(arena.CompleteReclaim());
    assert(arena.Reserve(4, false, d) && d == 0);
    assert(arena.Retire(c, 4));
    assert(arena.Retire(d, 4));
    assert(arena.BeginReclaim());
    assert(arena.CompleteReclaim());
    assert(arena.Reserve(8, false, d));
}

static void RandomReference() {
    constexpr unsigned pages = 257;
    VmallocArena<pages> arena;
    struct Allocation {uint64_t usable, count, first, total; unsigned phase;};
    std::vector<Allocation> allocations;
    unsigned char reference[pages]{};
    std::mt19937 random(42);
    bool barrier = false;
    unsigned generation = 1, batch = 0;
    for (unsigned step = 0; step < 100000; ++step) {
        const unsigned op = random() % 5;
        if (op < 2) {
            uint64_t count = 1 + random() % 40, usable;
            bool guards = random() % 2;
            const uint64_t total = count + (guards ? 2 : 0);
            unsigned run = 0; bool possible = false;
            for (unsigned i = 0; i < pages; ++i) {
                run = reference[i] ? 0 : run + 1;
                if (run >= total) possible = true;
            }
            const bool result = arena.Reserve(count, guards, usable);
            assert(result == possible);
            if (result) {
                const uint64_t first = usable - (guards ? 1 : 0);
                assert(first + total <= pages);
                for (uint64_t i = first; i < first + total; ++i) {
                    assert(!reference[i]); reference[i] = 1;
                }
                allocations.push_back({usable, count, first, total, 0});
            }
        } else if (op == 2 && !allocations.empty()) {
            auto& allocation = allocations[random() % allocations.size()];
            const bool result = arena.Retire(allocation.usable, allocation.count);
            assert(result == (allocation.phase == 0));
            if (result) allocation.phase = generation;
        } else if (op == 3) {
            bool pending = false;
            for (auto& allocation : allocations) if (allocation.phase == generation) pending = true;
            const bool result = arena.BeginReclaim();
            assert(result == (!barrier && pending));
            if (result) {barrier = true; batch = generation++;}
        } else if (op == 4) {
            assert(arena.CompleteReclaim() == barrier);
            if (barrier) {
                for (auto it = allocations.begin(); it != allocations.end();) {
                    if (it->phase != batch) {++it; continue;}
                    for (uint64_t i = it->first; i < it->first + it->total; ++i) reference[i] = 0;
                    it = allocations.erase(it);
                }
                barrier = false;
            }
        }
    }
}

int main() {
    ExhaustionAndGuards<1>();
    ExhaustionAndGuards<3>();
    ExhaustionAndGuards<65>();
    ExhaustionAndGuards<257>();
    BarrierGenerationAndCoalescing();
    RandomReference();
    std::puts("vmalloc arena: exhaustion, guards, generations and 100000 reference operations passed");
}

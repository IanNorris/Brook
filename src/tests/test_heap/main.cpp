#include "test_framework.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"

TEST_MAIN("heap", {
    brook::PmmInit(brook::test::g_protocol);
    brook::VmmInit();
    brook::HeapInit();

    // Exercise actual allocator timing with IF disabled, as during early boot.
    // Restore the caller's flags; this test does not install an interrupt table.
    uint64_t savedFlags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(savedFlags) :: "memory");
    auto irqEnabled = []() {
        uint64_t flags;
        __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
        return (flags & 512) != 0;
    };
    brook::HeapTimingStats timing;
    brook::HeapTimingSnapshot(&timing);
    ASSERT_FALSE(timing.enabled);
    brook::HeapTimingReset();
    brook::HeapTimingEnable(true);
    auto* timed = static_cast<uint8_t*>(brook::kmalloc(32));
    ASSERT_TRUE(timed != nullptr);
    ASSERT_FALSE(irqEnabled());
    timed[0] = 0x73;
    auto* grown = static_cast<uint8_t*>(brook::krealloc(timed, 512));
    ASSERT_TRUE(grown != nullptr && grown[0] == 0x73);
    ASSERT_FALSE(irqEnabled());
    brook::kfree(grown);
    ASSERT_FALSE(irqEnabled());
    brook::HeapTimingSnapshot(&timing);
    ASSERT_TRUE(timing.enabled && timing.allocations >= 1 && timing.frees >= 1);
    ASSERT_EQ(timing.reallocations, uint64_t{1});
    ASSERT_TRUE(timing.headers >= timing.allocations && timing.holdCycles > 0);
    brook::HeapTimingReset();
    brook::HeapTimingSnapshot(&timing);
    ASSERT_TRUE(timing.enabled);
    ASSERT_EQ(timing.allocations, uint64_t{0});
    ASSERT_EQ(timing.headers, uint64_t{0});
    ASSERT_EQ(timing.holdCycles, uint64_t{0});
    brook::HeapTimingEnable(false);
    brook::kfree(brook::kmalloc(32));
    brook::HeapTimingSnapshot(&timing);
    ASSERT_FALSE(timing.enabled);
    ASSERT_EQ(timing.allocations, uint64_t{0});
    ASSERT_EQ(timing.frees, uint64_t{0});
    char tinyReport[1] = {'x'};
    brook::HeapTimingFormat(tinyReport, sizeof(tinyReport));
    ASSERT_EQ(tinyReport[0], char{0});
    ASSERT_FALSE(irqEnabled());
    __asm__ volatile("push %0; popfq" :: "r"(savedFlags) : "memory", "cc");
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    brook::SerialPuts("Heap timing PASS off/reset/counters/realloc/IF-disabled/format-bound\n");

    // Runtime poison starts off. Enabling after frees must seed free regions
    // without overwriting live allocations or reporting ordinary data as UAF.
    brook::HeapStats stats;
    brook::HeapGetStats(&stats);
    ASSERT_TRUE(!stats.poisonEnabled);
    auto* left = static_cast<uint8_t*>(brook::kmalloc(64));
    auto* hole = static_cast<uint8_t*>(brook::kmalloc(64));
    auto* right = static_cast<uint8_t*>(brook::kmalloc(64));
    ASSERT_TRUE(left && hole && right);
    for (int i = 0; i < 64; ++i) {
        left[i] = 0x71; hole[i] = 0xA5; right[i] = 0x72;
    }
    brook::kfree(hole);
    brook::HeapSetPoison(true);
    brook::HeapGetStats(&stats);
    ASSERT_TRUE(stats.poisonEnabled);
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(left[i], static_cast<uint8_t>(0x71));
        ASSERT_EQ(right[i], static_cast<uint8_t>(0x72));
        ASSERT_EQ(hole[i], static_cast<uint8_t>(0xDF));
    }
    auto* seeded = static_cast<uint8_t*>(brook::kmalloc(64));
    ASSERT_TRUE(seeded != nullptr);
    for (int i = 0; i < 64; ++i)
        ASSERT_EQ(seeded[i], static_cast<uint8_t>(0xCD));
    brook::HeapSetPoison(false);
    brook::HeapGetStats(&stats);
    ASSERT_TRUE(!stats.poisonEnabled);
    brook::kfree(seeded);
    brook::kfree(left);
    brook::kfree(right);
    ASSERT_TRUE(brook::HeapCheckIntegrity());

    // --- Basic alloc/free ---
    void* a = brook::kmalloc(64);
    ASSERT_TRUE(a != nullptr);
    ASSERT_EQ(reinterpret_cast<uint64_t>(a) & 0xF, (uint64_t)0); // 16-byte aligned

    brook::kfree(a);

    // --- Freed block is reused ---
    void* b = brook::kmalloc(64);
    ASSERT_TRUE(b != nullptr);
    // Not guaranteed to be same pointer (could split/coalesce), but should be non-null.

    // --- 64 simultaneous allocations, all distinct and non-overlapping ---
    static void* ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = brook::kmalloc(128);
        ASSERT_TRUE(ptrs[i] != nullptr);
        ASSERT_EQ(reinterpret_cast<uint64_t>(ptrs[i]) & 0xF, (uint64_t)0);
    }
    // All distinct
    for (int i = 0; i < 64; i++) {
        for (int j = i + 1; j < 64; j++) {
            ASSERT_NE(ptrs[i], ptrs[j]);
        }
    }
    // Write pattern, verify no overlap corruption
    for (int i = 0; i < 64; i++) {
        uint8_t* p = static_cast<uint8_t*>(ptrs[i]);
        for (int b2 = 0; b2 < 128; b2++) p[b2] = static_cast<uint8_t>(i);
    }
    for (int i = 0; i < 64; i++) {
        uint8_t* p = static_cast<uint8_t*>(ptrs[i]);
        for (int b2 = 0; b2 < 128; b2++) {
            ASSERT_EQ(p[b2], static_cast<uint8_t>(i));
        }
    }

    // --- Free all 64 + b ---
    brook::kfree(b);
    for (int i = 0; i < 64; i++) brook::kfree(ptrs[i]);

    // --- Large allocation (>4KB, spans multiple pages via a single block) ---
    void* big = brook::kmalloc(8192);
    ASSERT_TRUE(big != nullptr);
    uint8_t* bigBytes = static_cast<uint8_t*>(big);
    for (int i = 0; i < 8192; i++) bigBytes[i] = static_cast<uint8_t>(i & 0xFF);
    for (int i = 0; i < 8192; i++) {
        ASSERT_EQ(bigBytes[i], static_cast<uint8_t>(i & 0xFF));
    }
    brook::kfree(big);

    // --- krealloc ---
    void* r = brook::kmalloc(32);
    ASSERT_TRUE(r != nullptr);
    uint8_t* rb = static_cast<uint8_t*>(r);
    for (int i = 0; i < 32; i++) rb[i] = static_cast<uint8_t>(i);
    void* r2 = brook::krealloc(r, 256);
    ASSERT_TRUE(r2 != nullptr);
    // Original bytes preserved
    uint8_t* rb2 = static_cast<uint8_t*>(r2);
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(rb2[i], static_cast<uint8_t>(i));
    }
    brook::kfree(r2);

    // Reject unrepresentable requests before mapping; failed realloc preserves
    // the original allocation and all its contents.
    auto* retained = static_cast<uint8_t*>(brook::kmalloc(64));
    ASSERT_TRUE(retained != nullptr);
    for (int i = 0; i < 64; ++i) retained[i] = static_cast<uint8_t>(i + 37);
    uint64_t oversized[3];
    oversized[0] = ~uint64_t{0};
    oversized[1] = 0xFFFFFFFFULL;
    oversized[2] = uint64_t{1} << 32;
    const uint64_t freeBeforeRejected = brook::HeapFreeBytes();
    for (uint64_t bytes : oversized) {
        ASSERT_TRUE(brook::kmalloc(bytes) == nullptr);
        ASSERT_TRUE(brook::krealloc(retained, bytes) == nullptr);
        for (int i = 0; i < 64; ++i)
            ASSERT_EQ(retained[i], static_cast<uint8_t>(i + 37));
        ASSERT_EQ(brook::HeapFreeBytes(), freeBeforeRejected);
        ASSERT_TRUE(brook::HeapCheckIntegrity());
    }
    brook::kfree(retained);

    // --- Stress: exceed the actual 1 MiB initial heap and measure expansion ---
    brook::HeapTimingReset();
    brook::HeapTimingEnable(true);
    static void* stress[1500];
    for (int i = 0; i < 1500; i++) {
        stress[i] = brook::kmalloc(1024);
        ASSERT_TRUE(stress[i] != nullptr);
    }
    for (int i = 0; i < 1500; i++) brook::kfree(stress[i]);
    brook::HeapTimingEnable(false);
    brook::HeapTimingSnapshot(&timing);
    ASSERT_TRUE(timing.expansions > 0 && timing.expansionHeaders > 0);
    ASSERT_TRUE(timing.expansionCycles > 0 && timing.maxExpansionCycles > 0);
    ASSERT_TRUE(brook::HeapCheckIntegrity());

    // Thousands of live blocks must not appear in allocation search counts.
    // Alternating holes exercise exact-bin removal and bidirectional coalescing.
    const uint64_t freeBeforeIndex = brook::HeapFreeBytes();
    static uint8_t* indexed[4096];
    for (unsigned i = 0; i < 4096; ++i) {
        indexed[i] = static_cast<uint8_t*>(brook::kmalloc(32));
        ASSERT_TRUE(indexed[i] != nullptr);
        ASSERT_EQ(reinterpret_cast<uint64_t>(indexed[i]) & 63, uint64_t{0});
        for (unsigned j = 0; j < 32; ++j) indexed[i][j] = static_cast<uint8_t>(i);
    }
    for (unsigned i = 0; i < 4096; i += 2) brook::kfree(indexed[i]);
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    // Reseeding free poison must not erase intrusive header links.
    brook::HeapSetPoison(true);
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    brook::HeapTimingReset();
    brook::HeapTimingEnable(true);
    for (unsigned i = 0; i < 4096; i += 2) {
        indexed[i] = static_cast<uint8_t*>(brook::kmalloc(32));
        ASSERT_TRUE(indexed[i] != nullptr);
    }
    brook::HeapTimingEnable(false);
    brook::HeapTimingSnapshot(&timing);
    ASSERT_EQ(timing.allocations, uint64_t{2048});
    ASSERT_EQ(timing.headers, uint64_t{2048});
    ASSERT_EQ(timing.maxHeaders, uint64_t{1});
    ASSERT_EQ(timing.expansions, uint64_t{0});
    brook::HeapSetPoison(false);
    for (unsigned i = 1; i < 4096; i += 2)
        for (unsigned j = 0; j < 32; ++j)
            ASSERT_EQ(indexed[i][j], static_cast<uint8_t>(i));
    // First free isolated odd blocks, then free evens to merge both sides.
    for (unsigned i = 1; i < 4096; i += 2) brook::kfree(indexed[i]);
    for (unsigned i = 0; i < 4096; i += 2) brook::kfree(indexed[i]);
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    ASSERT_EQ(brook::HeapFreeBytes(), freeBeforeIndex);

    // Exact minimum split and undersized remainder must both preserve tags.
    auto* guardA = brook::kmalloc(64);
    auto* splitHole = static_cast<uint8_t*>(brook::kmalloc(256));
    auto* guardB = brook::kmalloc(64);
    ASSERT_TRUE(guardA && splitHole && guardB);
    brook::kfree(splitHole);
    auto* splitA = static_cast<uint8_t*>(brook::kmalloc(64));
    auto* splitB = static_cast<uint8_t*>(brook::kmalloc(64));
    ASSERT_TRUE(splitA == splitHole && splitB == splitHole + 192);
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    brook::kfree(splitA); brook::kfree(splitB);
    auto* noSplit = brook::kmalloc(192); // 384-byte hole minus 320 < minimum block
    ASSERT_TRUE(noSplit == splitHole);
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    brook::kfree(noSplit); brook::kfree(guardA); brook::kfree(guardB);
    ASSERT_EQ(brook::HeapFreeBytes(), freeBeforeIndex);

    // Same floor-log2 class can contain both undersized and fitting blocks.
    // Free in reverse-size order so the search must skip the small head.
    auto* classGuardA = brook::kmalloc(64);
    auto* classLarge = brook::kmalloc(320); // total 448, bin 8
    auto* classGuardB = brook::kmalloc(64);
    auto* classSmall = brook::kmalloc(192); // total 320, bin 8
    auto* classGuardC = brook::kmalloc(64);
    ASSERT_TRUE(classGuardA && classLarge && classGuardB && classSmall && classGuardC);
    brook::kfree(classLarge); brook::kfree(classSmall);
    brook::HeapTimingReset(); brook::HeapTimingEnable(true);
    auto* classFit = brook::kmalloc(256); // total 384, skip 320 and select 448
    brook::HeapTimingEnable(false); brook::HeapTimingSnapshot(&timing);
    ASSERT_TRUE(classFit == classLarge);
    ASSERT_EQ(timing.headers, uint64_t{2});
    brook::kfree(classFit); brook::kfree(classGuardA);
    brook::kfree(classGuardB); brook::kfree(classGuardC);
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    ASSERT_EQ(brook::HeapFreeBytes(), freeBeforeIndex);

    // Deterministic mixed-class fragmentation/reallocation with byte oracle.
    static uint8_t* mixed[256] = {};
    static uint32_t lengths[256] = {};
    uint32_t random = 0x19760831;
    for (unsigned round = 0; round < 12000; ++round) {
        random = random * 1664525u + 1013904223u;
        const unsigned slot = (random >> 16) & 255;
        if (mixed[slot]) {
            for (unsigned j = 0; j < lengths[slot]; ++j)
                ASSERT_EQ(mixed[slot][j], static_cast<uint8_t>(slot + j));
        }
        random = random * 1664525u + 1013904223u;
        if (mixed[slot] && (random & 3) == 0) {
            brook::kfree(mixed[slot]); mixed[slot] = nullptr; lengths[slot] = 0;
        } else {
            const uint32_t length = 1 + ((random >> 8) & 2047);
            const uint32_t oldLength = lengths[slot];
            auto* replacement = static_cast<uint8_t*>(brook::krealloc(mixed[slot], length));
            ASSERT_TRUE(replacement != nullptr);
            for (unsigned j = 0; j < oldLength && j < length; ++j)
                ASSERT_EQ(replacement[j], static_cast<uint8_t>(slot + j));
            mixed[slot] = replacement; lengths[slot] = length;
            for (unsigned j = 0; j < length; ++j) replacement[j] = static_cast<uint8_t>(slot + j);
        }
        if ((round & 127) == 0) ASSERT_TRUE(brook::HeapCheckIntegrity());
    }
    for (unsigned i = 0; i < 256; ++i) brook::kfree(mixed[i]);
    ASSERT_TRUE(brook::HeapCheckIntegrity());
    ASSERT_EQ(brook::HeapFreeBytes(), freeBeforeIndex);
    brook::SerialPuts("Heap free-index PASS 4096-live/2048-one-header/splits/coalesces/poison/12000-mixed\n");

    brook::SerialPrintf("Heap test: %u bytes free after test\n",
                        (uint32_t)brook::HeapFreeBytes());
})

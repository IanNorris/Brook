#include "test_framework.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"

TEST_MAIN("heap", {
    brook::PmmInit(brook::test::g_protocol);
    brook::VmmInit();
    brook::HeapInit();

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

    // --- Stress: exhaust the initial 256KB heap, forcing expansion ---
    // Allocate 300 x 1KB = 300KB > 256KB initial. Heap must expand.
    static void* stress[300];
    for (int i = 0; i < 300; i++) {
        stress[i] = brook::kmalloc(1024);
        ASSERT_TRUE(stress[i] != nullptr);
    }
    for (int i = 0; i < 300; i++) brook::kfree(stress[i]);

    brook::SerialPrintf("Heap test: %u bytes free after test\n",
                        (uint32_t)brook::HeapFreeBytes());
})

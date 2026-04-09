#include "test_framework.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"

TEST_MAIN("pmm", {
    // --- Basic init ---
    brook::PmmInit(brook::test::g_protocol);

    uint64_t totalPages = brook::PmmGetTotalPageCount();
    uint64_t freePages  = brook::PmmGetFreePageCount();

    ASSERT_TRUE(totalPages > 0);
    ASSERT_TRUE(freePages  > 0);
    ASSERT_TRUE(freePages  < totalPages);   // some pages are reserved

    // --- Single allocation ---
    uint64_t p1 = brook::PmmAllocPage();
    ASSERT_TRUE(p1 != 0);
    ASSERT_EQ(p1 & 0xFFF, (uint64_t)0);    // 4KB-aligned

    uint64_t p2 = brook::PmmAllocPage();
    ASSERT_TRUE(p2 != 0);
    ASSERT_NE(p1, p2);                      // distinct pages

    // Free count should have decreased
    ASSERT_EQ(brook::PmmGetFreePageCount(), freePages - 2);

    // --- Free and re-allocate ---
    brook::PmmFreePage(p1);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freePages - 1);

    uint64_t p3 = brook::PmmAllocPage();
    ASSERT_TRUE(p3 != 0);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freePages - 2);

    // --- Alloc 8 pages, all distinct and aligned ---
    uint64_t pages[8];
    for (int i = 0; i < 8; i++) {
        pages[i] = brook::PmmAllocPage();
        ASSERT_TRUE(pages[i] != 0);
        ASSERT_EQ(pages[i] & 0xFFF, (uint64_t)0);
    }
    // Verify they are all distinct (O(n^2) but small n)
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            ASSERT_NE(pages[i], pages[j]);
        }
    }

    // --- Contiguous allocation ---
    uint64_t contiguous = brook::PmmAllocPages(16);
    ASSERT_TRUE(contiguous != 0);
    ASSERT_EQ(contiguous & 0xFFF, (uint64_t)0);

    // All pages in the run should now be used — verify by freeing and checking
    // we can re-allocate after freeing the whole run.
    for (uint64_t i = 0; i < 16; i++)
        brook::PmmFreePage(contiguous + i * 4096);

    uint64_t contiguous2 = brook::PmmAllocPages(16);
    ASSERT_TRUE(contiguous2 != 0);

    // --- Double-free safety (should not crash) ---
    brook::PmmFreePage(p2);
    brook::PmmFreePage(p2); // double-free, expect silent ignore

    // --- Page 0 is never returned (NULL guard) ---
    // We can't easily prove this directly, but we can assert 0 is used by
    // checking it's not returned in a fresh allocation sequence.
    // Cheapest proxy: p1..p3 were all non-zero (already asserted above).

    brook::SerialPrintf("PMM basic: %u/%u pages free\n",
                        (uint32_t)brook::PmmGetFreePageCount(),
                        (uint32_t)brook::PmmGetTotalPageCount());

    // -----------------------------------------------------------------------
    // Ownership tracking tests — requires full heap stack
    // -----------------------------------------------------------------------
    brook::VmmInit();
    brook::HeapInit();
    brook::PmmEnableTracking();

    // Allocate pages tagged for PID 1 and verify tracking.
    uint64_t tracked1 = brook::PmmAllocPage(brook::MemTag::KernelData, 1);
    ASSERT_TRUE(tracked1 != 0);
    ASSERT_EQ(brook::PmmGetTag(tracked1), brook::MemTag::KernelData);
    ASSERT_EQ(brook::PmmGetPid(tracked1), (uint16_t)1);

    uint64_t tracked2 = brook::PmmAllocPage(brook::MemTag::User, 1);
    ASSERT_TRUE(tracked2 != 0);
    ASSERT_EQ(brook::PmmGetTag(tracked2), brook::MemTag::User);
    ASSERT_EQ(brook::PmmGetPid(tracked2), (uint16_t)1);

    uint64_t freeBeforeKill = brook::PmmGetFreePageCount();

    // PmmKillPid should return all PID 1 pages to the free pool.
    brook::PmmKillPid(1);

    // Both tracked pages should now be free.
    ASSERT_EQ(brook::PmmGetFreePageCount(), freeBeforeKill + 2);
    ASSERT_EQ(brook::PmmGetTag(tracked1), brook::MemTag::Free);
    ASSERT_EQ(brook::PmmGetTag(tracked2), brook::MemTag::Free);
    ASSERT_EQ(brook::PmmGetPid(tracked1), (uint16_t)0);

    // PmmKillPid(0) = KernelPid must be a no-op.
    uint64_t freeAfterKillKernel = brook::PmmGetFreePageCount();
    brook::PmmKillPid(0);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freeAfterKillKernel);

    // PmmDumpPidStats must not crash.
    brook::PmmDumpPidStats();

    // Enumerate PID 0 kernel pages (just check it completes without crash).
    uint32_t enumCount = 0;
    brook::PmmEnumeratePid(0, [](uint64_t, brook::MemTag, void* ctx) -> bool {
        (*reinterpret_cast<uint32_t*>(ctx))++;
        return true;
    }, &enumCount);
    ASSERT_TRUE(enumCount > 0);

    brook::SerialPrintf("PMM tracking: %u/%u pages free, PID0 has %u pages\n",
                        (uint32_t)brook::PmmGetFreePageCount(),
                        (uint32_t)brook::PmmGetTotalPageCount(),
                        enumCount);
})


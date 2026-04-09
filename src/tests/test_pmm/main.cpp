#include "test_framework.h"
#include "pmm.h"

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

    brook::SerialPrintf("PMM test: %u/%u pages free after test\n",
                        (uint32_t)brook::PmmGetFreePageCount(),
                        (uint32_t)brook::PmmGetTotalPageCount());
})

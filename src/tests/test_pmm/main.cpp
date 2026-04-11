#include "test_framework.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"

using brook::PhysicalAddress;

TEST_MAIN("pmm", {
    // --- Basic init ---
    brook::PmmInit(brook::test::g_protocol);

    uint64_t totalPages = brook::PmmGetTotalPageCount();
    uint64_t freePages  = brook::PmmGetFreePageCount();

    ASSERT_TRUE(totalPages > 0);
    ASSERT_TRUE(freePages  > 0);
    ASSERT_TRUE(freePages  < totalPages);   // some pages are reserved

    // --- Single allocation ---
    PhysicalAddress p1 = brook::PmmAllocPage();
    ASSERT_TRUE(p1.raw() != 0);
    ASSERT_EQ(p1.raw() & 0xFFF, (uint64_t)0);    // 4KB-aligned

    PhysicalAddress p2 = brook::PmmAllocPage();
    ASSERT_TRUE(p2.raw() != 0);
    ASSERT_NE(p1.raw(), p2.raw());                // distinct pages

    // Free count should have decreased
    ASSERT_EQ(brook::PmmGetFreePageCount(), freePages - 2);

    // --- Free and re-allocate ---
    brook::PmmFreePage(p1);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freePages - 1);

    PhysicalAddress p3 = brook::PmmAllocPage();
    ASSERT_TRUE(p3.raw() != 0);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freePages - 2);

    // --- Alloc 8 pages, all distinct and aligned ---
    PhysicalAddress pages[8];
    for (int i = 0; i < 8; i++) {
        pages[i] = brook::PmmAllocPage();
        ASSERT_TRUE(pages[i].raw() != 0);
        ASSERT_EQ(pages[i].raw() & 0xFFF, (uint64_t)0);
    }
    // Verify they are all distinct (O(n^2) but small n)
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            ASSERT_NE(pages[i].raw(), pages[j].raw());
        }
    }

    // --- Contiguous allocation ---
    PhysicalAddress contiguous = brook::PmmAllocPages(16);
    ASSERT_TRUE(contiguous.raw() != 0);
    ASSERT_EQ(contiguous.raw() & 0xFFF, (uint64_t)0);

    // All pages in the run should now be used — verify by freeing and checking
    // we can re-allocate after freeing the whole run.
    for (uint64_t i = 0; i < 16; i++)
        brook::PmmFreePage(PhysicalAddress(contiguous.raw() + i * 4096));

    PhysicalAddress contiguous2 = brook::PmmAllocPages(16);
    ASSERT_TRUE(contiguous2.raw() != 0);

    // --- Double-free safety (should not crash) ---
    brook::PmmFreePage(p2);
    brook::PmmFreePage(p2); // double-free, expect silent ignore

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
    PhysicalAddress tracked1 = brook::PmmAllocPage(brook::MemTag::KernelData, 1);
    ASSERT_TRUE(tracked1.raw() != 0);
    ASSERT_EQ(brook::PmmGetTag(tracked1), brook::MemTag::KernelData);
    ASSERT_EQ(brook::PmmGetPid(tracked1), (uint16_t)1);

    PhysicalAddress tracked2 = brook::PmmAllocPage(brook::MemTag::User, 1);
    ASSERT_TRUE(tracked2.raw() != 0);
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
    brook::PmmEnumeratePid(0, [](PhysicalAddress, brook::MemTag, void* ctx) -> bool {
        (*reinterpret_cast<uint32_t*>(ctx))++;
        return true;
    }, &enumCount);
    ASSERT_TRUE(enumCount > 0);

    brook::SerialPrintf("PMM tracking: %u/%u pages free, PID0 has %u pages\n",
                        (uint32_t)brook::PmmGetFreePageCount(),
                        (uint32_t)brook::PmmGetTotalPageCount(),
                        enumCount);
})

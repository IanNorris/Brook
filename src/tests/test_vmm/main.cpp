#include "test_framework.h"
#include "pmm.h"
#include "vmm.h"

TEST_MAIN("vmm", {
    brook::PmmInit(brook::test::g_protocol);
    brook::VmmInit();

    // --- VmmAllocPages returns page-aligned non-zero address ---
    uint64_t p = brook::VmmAllocPages(1);
    ASSERT_TRUE(p != 0);
    ASSERT_EQ(p & 0xFFF, (uint64_t)0);

    // --- Write and read back across the page ---
    uint32_t* page = reinterpret_cast<uint32_t*>(p);
    for (int i = 0; i < 1024; i++) page[i] = static_cast<uint32_t>(0xDEAD0000 + i);
    for (int i = 0; i < 1024; i++) {
        ASSERT_EQ(page[i], static_cast<uint32_t>(0xDEAD0000 + i));
    }

    // --- VmmVirtToPhys returns consistent physical address ---
    uint64_t phys = brook::VmmVirtToPhys(p);
    ASSERT_TRUE(phys != 0);
    ASSERT_EQ(phys & 0xFFF, (uint64_t)0); // page-aligned

    // The physical page is also accessible via identity map (phys == virt):
    uint32_t* physPage = reinterpret_cast<uint32_t*>(phys);
    ASSERT_EQ(physPage[0], (uint32_t)0xDEAD0000);
    ASSERT_EQ(physPage[42], (uint32_t)(0xDEAD0000 + 42));

    // --- VmmMapPage: map the SAME physical page at a second virtual address ---
    uint64_t p2 = brook::VmmAllocPages(1);
    ASSERT_TRUE(p2 != 0);
    ASSERT_NE(p, p2); // distinct virtual addresses

    // Remap p2's virtual to p's physical.
    brook::VmmUnmapPage(p2);           // drop the page VmmAllocPages gave it
    bool ok = brook::VmmMapPage(p2, phys, brook::VMM_WRITABLE);
    ASSERT_TRUE(ok);

    uint32_t* alias = reinterpret_cast<uint32_t*>(p2);
    ASSERT_EQ(alias[0], (uint32_t)0xDEAD0000);  // same physical page

    // Write through alias, read back through original.
    alias[0] = 0xCAFEBABE;
    ASSERT_EQ(page[0], (uint32_t)0xCAFEBABE);

    // --- VmmFreePages releases physical pages back to PMM ---
    uint64_t freeBefore = brook::PmmGetFreePageCount();
    brook::VmmFreePages(p, 1);
    uint64_t freeAfter = brook::PmmGetFreePageCount();
    ASSERT_EQ(freeAfter, freeBefore + 1);

    // --- Multi-page allocation ---
    uint64_t multi = brook::VmmAllocPages(4);
    ASSERT_TRUE(multi != 0);
    ASSERT_EQ(multi & 0xFFF, (uint64_t)0);

    // All 4 pages should be accessible and distinct phys addresses.
    uint64_t prev_phys = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t virt = multi + (uint64_t)i * 4096;
        uint64_t ph   = brook::VmmVirtToPhys(virt);
        ASSERT_TRUE(ph != 0);
        ASSERT_NE(ph, prev_phys);
        prev_phys = ph;
    }

    brook::SerialPrintf("VMM test: VMALLOC next=0x%p\n",
        reinterpret_cast<void*>(brook::VMALLOC_BASE));
})

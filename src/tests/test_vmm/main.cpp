#include "test_framework.h"
#include "pmm.h"
#include "vmm.h"

TEST_MAIN("vmm", {
    brook::PmmInit(brook::test::g_protocol);
    brook::VmmInit();

    // --- Null guard: mapping below 1GB must be rejected ---
    bool nullMapped = brook::VmmMapPage(0, 0x1000, brook::VMM_WRITABLE);
    ASSERT_FALSE(nullMapped);
    bool lowMapped = brook::VmmMapPage(0x10000, 0x1000, brook::VMM_WRITABLE);
    ASSERT_FALSE(lowMapped);
    bool guardEdge = brook::VmmMapPage(brook::VIRTUAL_NULL_GUARD - 4096, 0x1000, brook::VMM_WRITABLE);
    ASSERT_FALSE(guardEdge);

    // --- VmmAllocPages returns page-aligned non-zero address (above guard) ---
    uint64_t p = brook::VmmAllocPages(1, brook::VMM_WRITABLE, brook::MemTag::KernelData);
    ASSERT_TRUE(p != 0);
    ASSERT_TRUE(p >= brook::VIRTUAL_NULL_GUARD);
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
    ASSERT_EQ(phys & 0xFFF, (uint64_t)0);

    // Physical page accessible via identity map (phys == virt):
    uint32_t* physPage = reinterpret_cast<uint32_t*>(phys);
    ASSERT_EQ(physPage[0], (uint32_t)0xDEAD0000);
    ASSERT_EQ(physPage[42], (uint32_t)(0xDEAD0000 + 42));

    // --- Alias: remap p2's virtual to the same physical page as p ---
    uint64_t p2 = brook::VmmAllocPages(1, brook::VMM_WRITABLE, brook::MemTag::KernelData);
    ASSERT_TRUE(p2 != 0);
    ASSERT_NE(p, p2);

    brook::VmmUnmapPage(p2);
    bool ok = brook::VmmMapPage(p2, phys, brook::VMM_WRITABLE);
    ASSERT_TRUE(ok);

    uint32_t* alias = reinterpret_cast<uint32_t*>(p2);
    ASSERT_EQ(alias[0], (uint32_t)0xDEAD0000);
    alias[0] = 0xCAFEBABE;
    ASSERT_EQ(page[0], (uint32_t)0xCAFEBABE);

    // --- PMM free count accounting ---
    uint64_t freeBefore = brook::PmmGetFreePageCount();
    brook::VmmFreePages(p, 1);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freeBefore + 1);

    // --- Multi-page allocation ---
    uint64_t multi = brook::VmmAllocPages(4, brook::VMM_WRITABLE, brook::MemTag::KernelData);
    ASSERT_TRUE(multi != 0);
    ASSERT_EQ(multi & 0xFFF, (uint64_t)0);

    uint64_t prev_phys = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t virt = multi + (uint64_t)i * 4096;
        uint64_t ph   = brook::VmmVirtToPhys(virt);
        ASSERT_TRUE(ph != 0);
        ASSERT_NE(ph, prev_phys);
        prev_phys = ph;
    }

    // --- VmmGetAllocation: registration table populated ---
    const brook::VmmAllocation* alloc = brook::VmmGetAllocation(multi);
    ASSERT_TRUE(alloc != nullptr);
    ASSERT_EQ(alloc->virtBase, multi);
    ASSERT_EQ(alloc->pageCount, (uint64_t)4);
    ASSERT_EQ((uint8_t)alloc->tag, (uint8_t)brook::MemTag::KernelData);

    brook::SerialPrintf("VMM test: null guard OK, alloc tracking OK\n");
})

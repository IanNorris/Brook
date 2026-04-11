#include "test_framework.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"

using brook::PhysicalAddress;
using brook::VirtualAddress;
using brook::KernelPageTable;

TEST_MAIN("vmm", {
    brook::PmmInit(brook::test::g_protocol);
    brook::VmmInit();

    // --- Null guard: mapping below 1GB must be rejected ---
    bool nullMapped = brook::VmmMapPage(KernelPageTable,
        VirtualAddress(0), PhysicalAddress(0x1000), brook::VMM_WRITABLE);
    ASSERT_FALSE(nullMapped);
    bool lowMapped = brook::VmmMapPage(KernelPageTable,
        VirtualAddress(0x8000), PhysicalAddress(0x1000), brook::VMM_WRITABLE);
    ASSERT_FALSE(lowMapped);
    bool guardEdge = brook::VmmMapPage(KernelPageTable,
        VirtualAddress(brook::VIRTUAL_NULL_GUARD - 4096),
        PhysicalAddress(0x1000), brook::VMM_WRITABLE);
    ASSERT_FALSE(guardEdge);

    // --- VmmAllocPages returns page-aligned non-zero address (above guard) ---
    VirtualAddress p = brook::VmmAllocPages(1, brook::VMM_WRITABLE, brook::MemTag::KernelData);
    ASSERT_TRUE(p.raw() != 0);
    ASSERT_TRUE(p.raw() >= brook::VIRTUAL_NULL_GUARD);
    ASSERT_EQ(p.raw() & 0xFFF, (uint64_t)0);

    // --- Write and read back across the page ---
    uint32_t* page = reinterpret_cast<uint32_t*>(p.raw());
    for (int i = 0; i < 1024; i++) page[i] = static_cast<uint32_t>(0xDEAD0000 + i);
    for (int i = 0; i < 1024; i++) {
        ASSERT_EQ(page[i], static_cast<uint32_t>(0xDEAD0000 + i));
    }

    // --- VmmVirtToPhys returns consistent physical address ---
    PhysicalAddress phys = brook::VmmVirtToPhys(KernelPageTable, p);
    ASSERT_TRUE(phys.raw() != 0);
    ASSERT_EQ(phys.raw() & 0xFFF, (uint64_t)0);

    // Physical page accessible via identity map (phys == virt):
    uint32_t* physPage = reinterpret_cast<uint32_t*>(phys.raw());
    ASSERT_EQ(physPage[0], (uint32_t)0xDEAD0000);
    ASSERT_EQ(physPage[42], (uint32_t)(0xDEAD0000 + 42));

    // --- Alias: remap p2's virtual to the same physical page as p ---
    VirtualAddress p2 = brook::VmmAllocPages(1, brook::VMM_WRITABLE, brook::MemTag::KernelData);
    ASSERT_TRUE(p2.raw() != 0);
    ASSERT_NE(p.raw(), p2.raw());

    brook::VmmUnmapPage(KernelPageTable, p2);
    bool ok = brook::VmmMapPage(KernelPageTable, p2, phys, brook::VMM_WRITABLE);
    ASSERT_TRUE(ok);

    uint32_t* alias = reinterpret_cast<uint32_t*>(p2.raw());
    ASSERT_EQ(alias[0], (uint32_t)0xDEAD0000);
    alias[0] = 0xCAFEBABE;
    ASSERT_EQ(page[0], (uint32_t)0xCAFEBABE);

    // --- PMM free count accounting ---
    uint64_t freeBefore = brook::PmmGetFreePageCount();
    brook::VmmFreePages(p, 1);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freeBefore + 1);

    // --- Multi-page allocation ---
    VirtualAddress multi = brook::VmmAllocPages(4, brook::VMM_WRITABLE, brook::MemTag::KernelData);
    ASSERT_TRUE(multi.raw() != 0);
    ASSERT_EQ(multi.raw() & 0xFFF, (uint64_t)0);

    uint64_t prev_phys = 0;
    for (int i = 0; i < 4; i++) {
        VirtualAddress virt(multi.raw() + (uint64_t)i * 4096);
        PhysicalAddress ph = brook::VmmVirtToPhys(KernelPageTable, virt);
        ASSERT_TRUE(ph.raw() != 0);
        ASSERT_NE(ph.raw(), prev_phys);
        prev_phys = ph.raw();
    }

    // --- VmmGetAllocation: registration table populated ---
    const brook::VmmAllocation* alloc = brook::VmmGetAllocation(multi);
    ASSERT_TRUE(alloc != nullptr);
    ASSERT_EQ(alloc->virtBase, multi.raw());
    ASSERT_EQ(alloc->pageCount, (uint64_t)4);
    ASSERT_EQ((uint8_t)alloc->tag, (uint8_t)brook::MemTag::KernelData);

    // -----------------------------------------------------------------------
    // PTE tag/PID encoding tests
    // -----------------------------------------------------------------------
    brook::HeapInit();
    brook::PmmEnableTracking();

    // Allocate with explicit tag and PID — encoded in PTE available bits.
    VirtualAddress tagged = brook::VmmAllocPages(2, brook::VMM_WRITABLE,
                                                 brook::MemTag::Heap, 42);
    ASSERT_TRUE(tagged.raw() != 0);

    // VmmGetPageTag/Pid must read back from the live PTE.
    ASSERT_EQ(brook::VmmGetPageTag(KernelPageTable, tagged), brook::MemTag::Heap);
    ASSERT_EQ(brook::VmmGetPageTag(KernelPageTable, VirtualAddress(tagged.raw() + 4096)),
              brook::MemTag::Heap);
    ASSERT_EQ(brook::VmmGetPagePid(KernelPageTable, tagged), (uint16_t)42);
    ASSERT_EQ(brook::VmmGetPagePid(KernelPageTable, VirtualAddress(tagged.raw() + 4096)),
              (uint16_t)42);

    // Unmapped address must return Free/KernelPid.
    ASSERT_EQ(brook::VmmGetPageTag(KernelPageTable, VirtualAddress(0xDEAD000000ULL)),
              brook::MemTag::Free);
    ASSERT_EQ(brook::VmmGetPagePid(KernelPageTable, VirtualAddress(0xDEAD000000ULL)),
              brook::KernelPid);

    // VmmKillPid frees virtual allocation and physical pages.
    uint64_t freeBeforeKill = brook::PmmGetFreePageCount();
    brook::VmmKillPid(42);
    ASSERT_EQ(brook::PmmGetFreePageCount(), freeBeforeKill + 2);

    // After kill, VmmGetAllocation should return nullptr for the freed range.
    ASSERT_TRUE(brook::VmmGetAllocation(tagged) == nullptr);

    brook::SerialPrintf("VMM test: null guard OK, PTE tag/pid roundtrip OK, "
                        "VmmKillPid OK\n");
})

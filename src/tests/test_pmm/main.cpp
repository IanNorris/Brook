#include "test_framework.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"

using brook::PhysicalAddress;

// Deterministic xorshift32 — bare-metal has no libc rand(). Fixed seed keeps the
// stress test reproducible so a failure is always replayable.
static uint32_t g_rngState = 0x9E3779B9u;
static uint32_t RngNext()
{
    uint32_t x = g_rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rngState = x;
    return x;
}

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

    // -----------------------------------------------------------------------
    // BRO-161 regression: a COW-shared page must NOT be freed when its
    // original owner exits while another process still references it.
    //
    // Ownership == membership in the owner's PID list (set at PmmAllocPage and
    // never transferred on share). On exit, the leader is torn down by TWO
    // paths: the page-table walk (FreeTableLevel -> PmmUnrefPage per mapped
    // PTE) and the PID-list sweep (PmmKillPid). For a shared page on the
    // owner's list, the unref drops refcount 2->1; PmmKillPid must then NOT
    // mistake the survivor for an exclusive page and free it out from under
    // the live co-owner.
    // -----------------------------------------------------------------------
    constexpr uint16_t PID_OWNER  = 10;  // original allocator (e.g. parent)
    constexpr uint16_t PID_SHARER = 11;  // COW co-owner (e.g. forked child)

    // Variant 1: owner exits while the page is still shared & still mapped.
    {
        PhysicalAddress shared = brook::PmmAllocPage(brook::MemTag::User, PID_OWNER);
        ASSERT_TRUE(shared.raw() != 0);
        brook::PmmRefPage(shared);                 // fork: child shares the page
        ASSERT_EQ(brook::PmmGetRefCount(shared), (uint8_t)2);

        // Owner teardown: page-table walk unrefs the owner's mapping...
        brook::PmmUnrefPage(shared);               // 2 -> 1
        ASSERT_EQ(brook::PmmGetRefCount(shared), (uint8_t)1);
        // ...then the PID-list sweep runs. It must skip the still-shared page.
        brook::PmmKillPid(PID_OWNER);

        // The sharer still references the page — it MUST remain allocated.
        ASSERT_NE(brook::PmmGetTag(shared), brook::MemTag::Free);
        ASSERT_EQ(brook::PmmGetRefCount(shared), (uint8_t)1);

        // Sharer exits last (page-table walk) — now it is genuinely freed.
        brook::PmmUnrefPage(shared);               // 1 -> 0, freed
        ASSERT_EQ(brook::PmmGetTag(shared), brook::MemTag::Free);
        brook::PmmKillPid(PID_SHARER);             // sharer list empty: no-op
    }

    // Variant 2: owner resolves its COW copy (stops mapping the shared page),
    // then exits. The shared page is still on the owner's PID list but no
    // longer in the owner's page table, so only PmmKillPid would reach it —
    // and it must not free it while the sharer is alive.
    {
        PhysicalAddress shared = brook::PmmAllocPage(brook::MemTag::User, PID_OWNER);
        ASSERT_TRUE(shared.raw() != 0);
        brook::PmmRefPage(shared);                 // shared owner+sharer
        ASSERT_EQ(brook::PmmGetRefCount(shared), (uint8_t)2);

        // Owner writes -> COW resolve: copies to a private page and drops its
        // reference on the shared one (without a page-table mapping remaining).
        PhysicalAddress copy = brook::PmmAllocPage(brook::MemTag::User, PID_OWNER);
        ASSERT_TRUE(copy.raw() != 0);
        brook::PmmUnrefPage(shared);               // 2 -> 1 (owner no longer maps it)
        ASSERT_EQ(brook::PmmGetRefCount(shared), (uint8_t)1);

        // Owner exits: page-table walk frees the private copy; PID-list sweep
        // must not free `shared`.
        brook::PmmUnrefPage(copy);                 // owner's private page: 1 -> 0
        ASSERT_EQ(brook::PmmGetTag(copy), brook::MemTag::Free);
        brook::PmmKillPid(PID_OWNER);

        ASSERT_NE(brook::PmmGetTag(shared), brook::MemTag::Free);
        ASSERT_EQ(brook::PmmGetRefCount(shared), (uint8_t)1);

        // Sharer exits: shared page finally freed.
        brook::PmmUnrefPage(shared);
        ASSERT_EQ(brook::PmmGetTag(shared), brook::MemTag::Free);
        brook::PmmKillPid(PID_SHARER);
    }

    brook::SerialPrintf("PMM BRO-161 COW-owner-exit: shared pages survived owner teardown\n");

    // -----------------------------------------------------------------------
    // BRO-161 randomized stress: N-way COW sharing with random exit order.
    //
    // This is the real trigger for the original crash — many processes forking
    // and exiting in arbitrary order while pages are shared 2..N ways. We model
    // the kernel's TWO teardown paths exactly:
    //   1. page-table walk: PmmUnrefPage() once per page the process maps;
    //   2. PID-list sweep:  PmmKillPid(pid).
    // and assert the core invariant after every operation:
    //   * a page with >=1 live mapper is NEVER freed, and its refcount equals
    //     the number of live mappers;
    //   * a page with 0 live mappers IS freed exactly once (no leak, no
    //     double-free).
    // A fixed RNG seed keeps any failure reproducible.
    // -----------------------------------------------------------------------
    {
        constexpr int      NPROC     = 6;
        constexpr uint16_t PID_BASE  = 20;          // PIDs 20..25 (well clear of others)
        constexpr int      MAXPAGES  = 48;
        constexpr int      ITERS     = 600;

        struct Pg {
            PhysicalAddress phys;
            uint32_t        mapperMask;             // bit i set => proc i maps it (live mappers only)
            bool            allocated;
        };
        Pg   pg[MAXPAGES];
        for (int i = 0; i < MAXPAGES; i++) { pg[i].mapperMask = 0; pg[i].allocated = false; }
        bool alive[NPROC];
        for (int i = 0; i < NPROC; i++) alive[i] = true;

        uint64_t freeBaseline = brook::PmmGetFreePageCount();

        auto verifyAll = [&]() {
            for (int s = 0; s < MAXPAGES; s++) {
                if (!pg[s].allocated) continue;
                int live = __builtin_popcount(pg[s].mapperMask);
                if (live == 0) {
                    // Last mapper exited: must be freed, then reclaim the slot.
                    ASSERT_EQ(brook::PmmGetTag(pg[s].phys), brook::MemTag::Free);
                    pg[s].allocated = false;
                } else {
                    ASSERT_NE(brook::PmmGetTag(pg[s].phys), brook::MemTag::Free);
                    ASSERT_EQ(brook::PmmGetRefCount(pg[s].phys), (uint8_t)live);
                }
            }
        };

        for (int it = 0; it < ITERS; it++) {
            uint32_t op = RngNext() % 3;

            if (op == 0) {
                // ALLOC: a live proc allocates a fresh private page it maps.
                int owner = RngNext() % NPROC;
                if (!alive[owner]) continue;
                int slot = -1;
                for (int s = 0; s < MAXPAGES; s++)
                    if (!pg[s].allocated) { slot = s; break; }
                if (slot < 0) continue;
                PhysicalAddress p = brook::PmmAllocPage(brook::MemTag::User,
                                                        (uint16_t)(PID_BASE + owner));
                ASSERT_TRUE(p.raw() != 0);
                ASSERT_EQ(brook::PmmGetRefCount(p), (uint8_t)1);
                pg[slot].phys       = p;
                pg[slot].mapperMask = (uint32_t)(1u << owner);
                pg[slot].allocated  = true;
            }
            else if (op == 1) {
                // SHARE (fork COW): a live proc that doesn't already map a
                // randomly-chosen page starts sharing it.
                int slot = RngNext() % MAXPAGES;
                if (!pg[slot].allocated) continue;
                int who = RngNext() % NPROC;
                if (!alive[who]) continue;
                if (pg[slot].mapperMask & (1u << who)) continue;   // already maps it
                if (__builtin_popcount(pg[slot].mapperMask) == 0) continue; // no live mapper to fork from
                brook::PmmRefPage(pg[slot].phys);
                pg[slot].mapperMask |= (1u << who);
            }
            else {
                // EXIT: a live proc tears down, then immediately respawns
                // (fresh empty address space, same PID — list must be clean).
                int who = RngNext() % NPROC;
                if (!alive[who]) continue;
                // Path 1: page-table walk unrefs every page this proc maps.
                for (int s = 0; s < MAXPAGES; s++) {
                    if (pg[s].allocated && (pg[s].mapperMask & (1u << who))) {
                        brook::PmmUnrefPage(pg[s].phys);
                        pg[s].mapperMask &= ~(1u << who);
                    }
                }
                // Path 2: PID-list sweep.
                brook::PmmKillPid((uint16_t)(PID_BASE + who));
                // Respawn: PID reused with an empty mapping set.
                alive[who] = true;
            }

            verifyAll();
        }

        // Final teardown: exit every proc, then assert no page leaked.
        for (int who = 0; who < NPROC; who++) {
            for (int s = 0; s < MAXPAGES; s++) {
                if (pg[s].allocated && (pg[s].mapperMask & (1u << who))) {
                    brook::PmmUnrefPage(pg[s].phys);
                    pg[s].mapperMask &= ~(1u << who);
                }
            }
            brook::PmmKillPid((uint16_t)(PID_BASE + who));
            alive[who] = false;
        }
        verifyAll();
        for (int s = 0; s < MAXPAGES; s++)
            ASSERT_TRUE(!pg[s].allocated);          // everything reclaimed

        ASSERT_EQ(brook::PmmGetFreePageCount(), freeBaseline);  // zero leak/double-free

        brook::SerialPrintf("PMM BRO-161 stress: %d iters, %d procs, N-way COW, "
                            "random exit order — no leak, no double-free\n",
                            ITERS, NPROC);
    }
})

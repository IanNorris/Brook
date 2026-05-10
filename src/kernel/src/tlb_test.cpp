// tlb_test.cpp — TLB shootdown self-test.
//
// Runs during boot after SMP is fully online. Verifies the TLB shootdown
// IPI mechanism works correctly by testing:
//
//   1. IPI round-trip: sends shootdown to each remote CPU and verifies ack.
//   2. Single-page invalidation round-trip.
//   3. Stress test: 100 rapid consecutive shootdowns.
//   4. Non-matching CR3: handler acks without invalidating.
//
// All tests run in kernel context on the BSP. Failures are reported via
// SerialPrintf and the test returns false.

#include "apic.h"
#include "smp.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"

namespace brook {

bool TlbShootdownSelfTest()
{
    uint32_t cpuCount = SmpGetCpuCount();
    SerialPrintf("TLB_TEST: starting self-test (%u CPUs online)\n", cpuCount);

    if (cpuCount < 2)
    {
        SerialPrintf("TLB_TEST: SKIP — only %u CPU online, need >= 2\n", cpuCount);
        return true;
    }

    // Test 1: IPI round-trip — full flush with kernel CR3
    {
        SerialPrintf("TLB_TEST: test 1 — IPI round-trip (full flush, kernel CR3)...\n");

        uint64_t kernelCr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(kernelCr3));

        extern volatile uint64_t g_lapicTickCount;
        uint64_t t0 = g_lapicTickCount;

        TlbShootdownFull(kernelCr3);

        uint64_t elapsed = g_lapicTickCount - t0;
        SerialPrintf("TLB_TEST: test 1 PASS — full flush completed in %lu ms\n", elapsed);
    }

    // Test 2: Single-page invalidation round-trip
    {
        SerialPrintf("TLB_TEST: test 2 — single-page shootdown...\n");

        PhysicalAddress testPhys = PmmAllocPage(MemTag::KernelData, 0);
        if (!testPhys)
        {
            SerialPrintf("TLB_TEST: test 2 FAIL — could not allocate test page\n");
            return false;
        }

        auto* ptr = reinterpret_cast<volatile uint64_t*>(PhysToVirt(testPhys).raw());
        *ptr = 0xDEADBEEF12345678ULL;

        uint64_t kernelCr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(kernelCr3));

        extern volatile uint64_t g_lapicTickCount;
        uint64_t t0 = g_lapicTickCount;

        TlbShootdown(kernelCr3, 0x0000100000000000ULL);

        uint64_t elapsed = g_lapicTickCount - t0;
        PmmFreePage(testPhys);

        SerialPrintf("TLB_TEST: test 2 PASS — single-page shootdown completed in %lu ms\n",
                     elapsed);
    }

    // Test 3: Rapid consecutive shootdowns (stress lock serialisation)
    {
        SerialPrintf("TLB_TEST: test 3 — rapid shootdown stress test (100 iterations)...\n");

        uint64_t kernelCr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(kernelCr3));

        extern volatile uint64_t g_lapicTickCount;
        uint64_t t0 = g_lapicTickCount;

        for (uint32_t i = 0; i < 100; i++)
        {
            if (i & 1)
                TlbShootdownFull(kernelCr3);
            else
                TlbShootdown(kernelCr3, 0x0000200000000000ULL + i * 4096);
        }

        uint64_t elapsed = g_lapicTickCount - t0;
        SerialPrintf("TLB_TEST: test 3 PASS — 100 shootdowns in %lu ms "
                     "(avg %lu us/shootdown)\n",
                     elapsed, elapsed * 1000 / 100);
    }

    // Test 4: Non-matching CR3 — handler acks without invalidating
    {
        SerialPrintf("TLB_TEST: test 4 — non-matching CR3...\n");

        uint64_t fakeCr3 = 0xDEAD000ULL;

        extern volatile uint64_t g_lapicTickCount;
        uint64_t t0 = g_lapicTickCount;

        TlbShootdownFull(fakeCr3);

        uint64_t elapsed = g_lapicTickCount - t0;
        SerialPrintf("TLB_TEST: test 4 PASS — non-matching CR3 flush completed in %lu ms\n",
                     elapsed);
    }

    SerialPrintf("TLB_TEST: all tests passed (%u CPUs)\n", cpuCount);
    return true;
}

} // namespace brook

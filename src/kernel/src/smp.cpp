#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "gdt.h"
#include "serial.h"
#include "kprintf.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"

#include <stdint.h>

// Trampoline start/end symbols (defined in ap_trampoline.S)
extern "C" {
    extern uint8_t _ap_trampoline_start[];
    extern uint8_t _ap_trampoline_end[];
    extern uint8_t _ap_data_block[];
}

namespace brook {

// ---------------------------------------------------------------------------
// Data block layout — must match ap_trampoline.S _ap_data_block
// ---------------------------------------------------------------------------
struct APDataBlock {
    uint32_t pml4;           // [+0]  CR3 (PML4 physical, 32-bit)
    uint32_t tmpStack;       // [+4]  32-bit temp stack pointer
    uint64_t stack64;        // [+8]  64-bit kernel stack top
    uint64_t entryFn;        // [+16] 64-bit C++ entry function
    uint16_t gdtLimit;       // [+24] GDT limit
    uint64_t gdtBase;        // [+26] GDT base
    uint16_t idtLimit;       // [+34] IDT limit
    uint64_t idtBase;        // [+36] IDT base
} __attribute__((packed));

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static CpuInfo     g_cpus[MAX_CPUS];
static uint32_t    g_cpuCount    = 0;
static uint32_t    g_onlineCount = 0;

// Volatile signal: set by AP to tell BSP it's alive.
static volatile uint32_t g_apSignal = 0;

// LAPIC helpers (re-declared locally to avoid exposing statics from apic.cpp)
static inline uint32_t LapicRead(uint32_t offset)
{
    uint64_t base = ApicGetLapicVirtBase();
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

static inline void LapicWrite(uint32_t offset, uint32_t val)
{
    uint64_t base = ApicGetLapicVirtBase();
    *reinterpret_cast<volatile uint32_t*>(base + offset) = val;
}

// ---------------------------------------------------------------------------
// AP Entry — called by each AP after reaching 64-bit mode
// ---------------------------------------------------------------------------

// Forward declaration for asm-safe linkage
extern "C" void ApEntryFunction();

extern "C" __attribute__((used))
void ApEntryFunction()
{
    // Enable LAPIC on this core
    uint64_t apicBase = ReadMsr(0x1B); // IA32_APIC_BASE
    apicBase |= (1ULL << 11);         // global enable
    WriteMsr(0x1B, apicBase);

    // Software-enable the LAPIC (SVR register)
    // The LAPIC is already MMIO-mapped by the BSP at a shared virtual address.
    LapicWrite(LapicReg::SVR, (1u << 8) | 0xFF); // enable + spurious vector
    LapicWrite(LapicReg::TPR, 0);                 // accept all priorities

    // Enable FPU/SSE on this core
    // CR0: clear EM (bit 2), set MP (bit 1), set NE (bit 5)
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);  // clear EM
    cr0 |=  (1ULL << 1);  // set MP
    cr0 |=  (1ULL << 5);  // set NE
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    // CR4: set OSFXSR (bit 9) + OSXMMEXCPT (bit 10)
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    uint8_t myId = static_cast<uint8_t>(LapicRead(LapicReg::ID) >> 24);
    SerialPrintf("SMP: AP %u online\n", myId);

    // Signal BSP that we're alive
    __atomic_store_n(&g_apSignal, 1, __ATOMIC_RELEASE);

    // Halt — APs will be woken later when scheduling is extended to SMP.
    // Keep interrupts disabled: we don't have per-CPU TSS/GDT yet.
    for (;;)
        __asm__ volatile("hlt" ::: "memory");
}

// ---------------------------------------------------------------------------
// IPI helpers
// ---------------------------------------------------------------------------

static bool WaitForIpiIdle()
{
    for (uint32_t i = 0; i < 100000; ++i)
    {
        if (!(LapicRead(LapicReg::ICR_LO) & (1u << 12)))
            return true;
        __asm__ volatile("pause" ::: "memory");
    }
    KPrintf("SMP:   WaitForIpiIdle TIMEOUT (ICR_LO=0x%x)\n",
            LapicRead(LapicReg::ICR_LO));
    return false;
}

static void BusySleepMs(uint32_t ms)
{
    // Use the LAPIC timer current count for timing.
    // The initial count register reloads automatically; read the current count.
    // Timer ticks per ms was calibrated by ApicInit.
    uint32_t ticksPerMs = ApicGetTimerTicksPerMs();
    for (uint32_t m = 0; m < ms; ++m)
    {
        uint32_t start = LapicRead(0x390); // LAPIC Current Count
        // Count decrements to 0 then reloads. Wait for enough ticks.
        uint32_t elapsed = 0;
        uint32_t prev = start;
        while (elapsed < ticksPerMs)
        {
            __asm__ volatile("pause" ::: "memory");
            uint32_t cur = LapicRead(0x390);
            if (cur <= prev)
                elapsed += (prev - cur);
            else
                elapsed += prev; // wrapped around
            prev = cur;
        }
    }
}

static void BusySleepUs(uint32_t us)
{
    // Approximate: spin for ~us microseconds using a calibrated loop.
    // At ~1GHz, ~1000 iterations per us. Conservative estimate.
    for (uint32_t i = 0; i < us * 100; ++i)
        __asm__ volatile("pause" ::: "memory");
}

// ---------------------------------------------------------------------------
// Boot one AP
// ---------------------------------------------------------------------------

static bool BootAP(uint8_t apicId, uint8_t sipiVector)
{
    // Clear error status
    LapicWrite(0x280, 0); // ESR
    LapicRead(0x280);

    // Send INIT IPI (assert + de-assert)
    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
    LapicWrite(LapicReg::ICR_LO, 0x00004500); // INIT | level assert
    WaitForIpiIdle();

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
    LapicWrite(LapicReg::ICR_LO, 0x00008500); // INIT | level de-assert
    WaitForIpiIdle();

    BusySleepMs(10);

    // Send SIPI twice (Intel recommendation)
    for (int i = 0; i < 2; ++i)
    {
        LapicWrite(0x280, 0); // clear ESR
        LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
        LapicWrite(LapicReg::ICR_LO, 0x00000600 | sipiVector); // SIPI
        BusySleepUs(200);
        WaitForIpiIdle();
    }

    // Wait for AP to signal (up to 200ms)
    for (uint32_t t = 0; t < 200; ++t)
    {
        if (__atomic_load_n(&g_apSignal, __ATOMIC_ACQUIRE))
            return true;
        BusySleepMs(1);
    }

    return false;
}

// ---------------------------------------------------------------------------
// SmpInit — main entry point
// ---------------------------------------------------------------------------

uint32_t SmpInit()
{
    const MadtInfo& madt = AcpiGetMadt();
    uint8_t bspId = ApicGetId();

    KPrintf("SMP: BSP LAPIC ID=%u, %u processor(s) in MADT\n",
                 bspId, madt.processorCount);

    if (madt.processorCount <= 1)
    {
        // Single CPU — nothing to do
        g_cpus[0].apicId = bspId;
        g_cpus[0].isBsp  = true;
        g_cpus[0].online = true;
        g_cpuCount = 1;
        g_onlineCount = 1;
        return 1;
    }

    // Record all CPUs
    g_cpuCount = madt.processorCount;
    for (uint32_t i = 0; i < g_cpuCount && i < MAX_CPUS; ++i)
    {
        g_cpus[i].apicId = madt.apicIds[i];
        g_cpus[i].isBsp  = (madt.apicIds[i] == bspId);
        g_cpus[i].online = g_cpus[i].isBsp; // BSP is already online
    }
    g_onlineCount = 1; // BSP

    // --- Set up trampoline page in low memory ---
    // The SIPI vector is an 8-bit value: startup_addr = vector * 0x1000.
    // We need to find a free physical page in the 0x1000-0xFF000 range.
    // Use a well-known address: 0x8000 (page 8).
    static constexpr uint64_t TRAMPOLINE_PHYS = 0x8000;
    static constexpr uint8_t  SIPI_VECTOR     = TRAMPOLINE_PHYS / 0x1000;

    // Access the trampoline page via the direct physical map.
    uint8_t* trampolineDst = reinterpret_cast<uint8_t*>(
        PhysToVirt(PhysicalAddress(TRAMPOLINE_PHYS)).raw());

    // Calculate trampoline size and data block offset
    uint32_t trampolineSize = static_cast<uint32_t>(
        _ap_trampoline_end - _ap_trampoline_start);
    uint32_t dataBlockOffset = static_cast<uint32_t>(
        _ap_data_block - _ap_trampoline_start);

    KPrintf("SMP: trampoline %u bytes, data block at offset %u\n",
                 trampolineSize, dataBlockOffset);

    if (trampolineSize > 4096)
    {
        KPuts("SMP: trampoline too large!\n");
        return 1;
    }

    // Ensure the trampoline page is mapped (identity mapping from bootloader
    // may still be present, but we also have the direct map).
    // Also ensure it's identity-mapped at 0x8000 for the 16-bit real mode code.
    // The bootloader typically identity-maps the first few MB.
    // Verify by checking if the direct map page is accessible.

    // Get the kernel CR3 (PML4 physical address) for the trampoline
    uint64_t kernelCR3 = VmmKernelCR3().pml4.raw();

    // We also need the trampoline page identity-mapped (physical = virtual)
    // for the real-mode code before paging is fully set up on the AP.
    // Map 0x8000 → 0x8000 in the kernel page table.
    VmmMapPage(VmmKernelCR3(), VirtualAddress(TRAMPOLINE_PHYS),
               PhysicalAddress(TRAMPOLINE_PHYS),
               VMM_WRITABLE | VMM_FORCE_MAP, MemTag::Device, KernelPid);

    // Also need a small temp stack for the AP's 32-bit code.
    // Use the page just below the trampoline: 0x7000 (top = 0x7FF0)
    static constexpr uint64_t TEMP_STACK_PHYS = 0x7000;
    VmmMapPage(VmmKernelCR3(), VirtualAddress(TEMP_STACK_PHYS),
               PhysicalAddress(TEMP_STACK_PHYS),
               VMM_WRITABLE | VMM_FORCE_MAP, MemTag::Device, KernelPid);

    // Get GDT and IDT descriptors from the BSP
    struct DescriptorTable {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed));

    DescriptorTable gdtDesc, idtDesc;
    __asm__ volatile("sgdt %0" : "=m"(gdtDesc));
    __asm__ volatile("sidt %0" : "=m"(idtDesc));

    // --- Boot each AP ---
    for (uint32_t i = 0; i < g_cpuCount && i < MAX_CPUS; ++i)
    {
        if (g_cpus[i].isBsp)
            continue;

        uint8_t apicId = g_cpus[i].apicId;

        // Allocate a per-AP kernel stack (16KB)
        static constexpr uint64_t AP_STACK_PAGES = 4;
        VirtualAddress stackBase = VmmAllocPages(AP_STACK_PAGES,
            VMM_WRITABLE, MemTag::KernelData, KernelPid);
        if (!stackBase)
        {
            KPrintf("SMP: failed to allocate stack for AP %u\n", apicId);
            continue;
        }
        uint64_t stackTop = stackBase.raw() + AP_STACK_PAGES * 4096 - 16;
        g_cpus[i].kernelStack = stackTop;

        // Copy trampoline to low memory
        for (uint32_t b = 0; b < trampolineSize; ++b)
            trampolineDst[b] = _ap_trampoline_start[b];

        // Patch the data block
        auto* data = reinterpret_cast<APDataBlock*>(trampolineDst + dataBlockOffset);
        data->pml4     = static_cast<uint32_t>(kernelCR3);
        data->tmpStack = static_cast<uint32_t>(TEMP_STACK_PHYS + 0xFF0);
        data->stack64  = stackTop;
        data->entryFn  = reinterpret_cast<uint64_t>(&ApEntryFunction);
        data->gdtLimit = gdtDesc.limit;
        data->gdtBase  = gdtDesc.base;
        data->idtLimit = idtDesc.limit;
        data->idtBase  = idtDesc.base;

        // Flush caches (ensure AP sees the patched data)
        __asm__ volatile("wbinvd" ::: "memory");

        // Send INIT + SIPI
        __atomic_store_n(&g_apSignal, 0, __ATOMIC_RELEASE);
        KPrintf("SMP: booting AP %u (LAPIC ID %u)...\n", i, apicId);

        if (BootAP(apicId, SIPI_VECTOR))
        {
            g_cpus[i].online = true;
            g_onlineCount++;
            KPrintf("SMP: AP %u (LAPIC ID %u) online ✓\n", i, apicId);
        }
        else
        {
            KPrintf("SMP: AP %u (LAPIC ID %u) failed to respond\n", i, apicId);
        }
    }

    // Clean up: unmap the identity-mapped low pages (no longer needed)
    VmmUnmapPage(VmmKernelCR3(), VirtualAddress(TRAMPOLINE_PHYS));
    VmmUnmapPage(VmmKernelCR3(), VirtualAddress(TEMP_STACK_PHYS));

    KPrintf("SMP: %u/%u CPUs online\n", g_onlineCount, g_cpuCount);
    return g_onlineCount;
}

uint32_t SmpGetCpuCount()
{
    return g_onlineCount;
}

const CpuInfo* SmpGetCpu(uint32_t index)
{
    if (index >= g_cpuCount) return nullptr;
    return &g_cpus[index];
}

uint32_t SmpCurrentCpuIndex()
{
    uint8_t myId = ApicGetId();
    for (uint32_t i = 0; i < g_cpuCount; ++i)
    {
        if (g_cpus[i].apicId == myId)
            return i;
    }
    return 0; // shouldn't happen
}

} // namespace brook

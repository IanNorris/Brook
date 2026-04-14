#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "gdt.h"
#include "serial.h"
#include "kprintf.h"
#include "scheduler.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/heap.h"

#include <stdint.h>

// LSTAR entry point (defined in syscall.cpp).
extern "C" void BrookSyscallDispatcher();

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

// Per-AP activation flag: BSP sets to 1 to tell AP to proceed.
static volatile uint32_t g_apActivate[MAX_CPUS] = {};

// Per-AP TSS selector (set by BSP, read by AP).
static uint16_t g_apTssSelector[MAX_CPUS] = {};

// Per-AP KernelCpuEnv (allocated by BSP, used by AP).
static KernelCpuEnv* g_apEnv[MAX_CPUS] = {};

// LAPIC helpers
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

extern "C" void ApEntryFunction();

extern "C" __attribute__((used))
void ApEntryFunction()
{
    // Enable LAPIC on this core
    uint64_t apicBase = ReadMsr(0x1B);
    apicBase |= (1ULL << 11);
    WriteMsr(0x1B, apicBase);

    LapicWrite(LapicReg::SVR, (1u << 8) | 0xFF);
    LapicWrite(LapicReg::TPR, 0);

    // Match BSP CR0: PG | WP | NE | ET | MP | PE, clear CD/NW/EM/TS.
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ULL << 2) |  // EM — clear for FPU
             (1ULL << 3) |   // TS — clear
             (1ULL << 29) |  // NW — clear (enable write-through)
             (1ULL << 30));  // CD — clear (enable caching)
    cr0 |=  (1ULL << 1) |   // MP
            (1ULL << 5) |    // NE
            (1ULL << 16);    // WP — write-protect
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    // Match BSP CR4: DE | PAE | MCE | OSFXSR | OSXMMEXCPT.
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 3) |    // DE
           (1ULL << 6) |    // MCE
           (1ULL << 9) |    // OSFXSR
           (1ULL << 10);    // OSXMMEXCPT
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    uint8_t myId = static_cast<uint8_t>(LapicRead(LapicReg::ID) >> 24);
    SerialPrintf("SMP: AP %u online\n", myId);

    // Determine our CPU index.
    uint32_t myCpuIndex = 0;
    for (uint32_t i = 0; i < g_cpuCount; ++i)
    {
        if (g_cpus[i].apicId == myId)
        {
            myCpuIndex = i;
            break;
        }
    }

    // Signal BSP that we're alive
    __atomic_store_n(&g_apSignal, 1, __ATOMIC_RELEASE);

    // Spin-wait for activation from BSP.
    while (!__atomic_load_n(&g_apActivate[myCpuIndex], __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");

    // ---- Per-CPU init ----

    // Load per-CPU GDT/TSS.
    GdtLoadOnAp(g_apTssSelector[myCpuIndex]);

    // Set up KernelCpuEnv and GS base for SWAPGS.
    KernelCpuEnv* env = g_apEnv[myCpuIndex];
    CpuSetKernelGsBase(env);

    // Set up SYSCALL MSRs.
    CpuInitSyscallMsrs(reinterpret_cast<uint64_t>(&BrookSyscallDispatcher));

    // Start LAPIC timer on this AP.
    ApicInitTimerOnAp();

    SerialPrintf("SMP: AP %u (CPU %u) activated, entering scheduler\n",
                 myId, myCpuIndex);

    // Enter scheduler loop (never returns).
    SchedulerStartAp();
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
    uint32_t ticksPerMs = ApicGetTimerTicksPerMs();
    for (uint32_t m = 0; m < ms; ++m)
    {
        uint32_t start = LapicRead(0x390);
        uint32_t elapsed = 0;
        uint32_t prev = start;
        while (elapsed < ticksPerMs)
        {
            __asm__ volatile("pause" ::: "memory");
            uint32_t cur = LapicRead(0x390);
            if (cur <= prev)
                elapsed += (prev - cur);
            else
                elapsed += prev;
            prev = cur;
        }
    }
}

static void BusySleepUs(uint32_t us)
{
    for (uint32_t i = 0; i < us * 100; ++i)
        __asm__ volatile("pause" ::: "memory");
}

// ---------------------------------------------------------------------------
// Boot one AP
// ---------------------------------------------------------------------------

static bool BootAP(uint8_t apicId, uint8_t sipiVector)
{
    LapicWrite(0x280, 0);
    LapicRead(0x280);

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
    LapicWrite(LapicReg::ICR_LO, 0x00004500);
    WaitForIpiIdle();

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
    LapicWrite(LapicReg::ICR_LO, 0x00008500);
    WaitForIpiIdle();

    BusySleepMs(10);

    for (int i = 0; i < 2; ++i)
    {
        LapicWrite(0x280, 0);
        LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
        LapicWrite(LapicReg::ICR_LO, 0x00000600 | sipiVector);
        BusySleepUs(200);
        WaitForIpiIdle();
    }

    for (uint32_t t = 0; t < 200; ++t)
    {
        if (__atomic_load_n(&g_apSignal, __ATOMIC_ACQUIRE))
            return true;
        BusySleepMs(1);
    }

    return false;
}

// ---------------------------------------------------------------------------
// SmpInit — main entry point (boots APs, they spin-wait for activation)
// ---------------------------------------------------------------------------

uint32_t SmpInit()
{
    const MadtInfo& madt = AcpiGetMadt();
    uint8_t bspId = ApicGetId();

    KPrintf("SMP: BSP LAPIC ID=%u, %u processor(s) in MADT\n",
                 bspId, madt.processorCount);

    if (madt.processorCount <= 1)
    {
        g_cpus[0].apicId = bspId;
        g_cpus[0].isBsp  = true;
        g_cpus[0].online = true;
        g_cpuCount = 1;
        g_onlineCount = 1;
        return 1;
    }

    g_cpuCount = madt.processorCount;
    for (uint32_t i = 0; i < g_cpuCount && i < MAX_CPUS; ++i)
    {
        g_cpus[i].apicId = madt.apicIds[i];
        g_cpus[i].isBsp  = (madt.apicIds[i] == bspId);
        g_cpus[i].online = g_cpus[i].isBsp;
    }
    g_onlineCount = 1;

    static constexpr uint64_t TRAMPOLINE_PHYS = 0x8000;
    static constexpr uint8_t  SIPI_VECTOR     = TRAMPOLINE_PHYS / 0x1000;

    uint8_t* trampolineDst = reinterpret_cast<uint8_t*>(
        PhysToVirt(PhysicalAddress(TRAMPOLINE_PHYS)).raw());

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

    uint64_t kernelCR3 = VmmKernelCR3().pml4.raw();

    VmmMapPage(VmmKernelCR3(), VirtualAddress(TRAMPOLINE_PHYS),
               PhysicalAddress(TRAMPOLINE_PHYS),
               VMM_WRITABLE | VMM_FORCE_MAP, MemTag::Device, KernelPid);

    static constexpr uint64_t TEMP_STACK_PHYS = 0x7000;
    VmmMapPage(VmmKernelCR3(), VirtualAddress(TEMP_STACK_PHYS),
               PhysicalAddress(TEMP_STACK_PHYS),
               VMM_WRITABLE | VMM_FORCE_MAP, MemTag::Device, KernelPid);

    struct DescriptorTable {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed));

    DescriptorTable gdtDesc, idtDesc;
    __asm__ volatile("sgdt %0" : "=m"(gdtDesc));
    __asm__ volatile("sidt %0" : "=m"(idtDesc));

    for (uint32_t i = 0; i < g_cpuCount && i < MAX_CPUS; ++i)
    {
        if (g_cpus[i].isBsp)
            continue;

        uint8_t apicId = g_cpus[i].apicId;

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

        for (uint32_t b = 0; b < trampolineSize; ++b)
            trampolineDst[b] = _ap_trampoline_start[b];

        auto* data = reinterpret_cast<APDataBlock*>(trampolineDst + dataBlockOffset);
        data->pml4     = static_cast<uint32_t>(kernelCR3);
        data->tmpStack = static_cast<uint32_t>(TEMP_STACK_PHYS + 0xFF0);
        data->stack64  = stackTop;
        data->entryFn  = reinterpret_cast<uint64_t>(&ApEntryFunction);
        data->gdtLimit = gdtDesc.limit;
        data->gdtBase  = gdtDesc.base;
        data->idtLimit = idtDesc.limit;
        data->idtBase  = idtDesc.base;

        __asm__ volatile("wbinvd" ::: "memory");

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

    VmmUnmapPage(VmmKernelCR3(), VirtualAddress(TRAMPOLINE_PHYS));
    VmmUnmapPage(VmmKernelCR3(), VirtualAddress(TEMP_STACK_PHYS));

    KPrintf("SMP: %u/%u CPUs online\n", g_onlineCount, g_cpuCount);
    return g_onlineCount;
}

// ---------------------------------------------------------------------------
// SmpActivateAPs — set up per-CPU state and wake APs into scheduler
// ---------------------------------------------------------------------------

// Forward-declare the syscall table getter (defined in syscall.cpp).
uint64_t SyscallGetTableAddress();

void SmpActivateAPs()
{
    if (g_onlineCount <= 1)
        return;

    KPrintf("SMP: activating %u APs for scheduling\n", g_onlineCount - 1);

    // Phase 1: prepare all APs (allocate resources while no APs are running).
    // This avoids heap/VMM lock contention between the BSP and active APs.
    for (uint32_t i = 0; i < g_cpuCount && i < MAX_CPUS; ++i)
    {
        if (g_cpus[i].isBsp || !g_cpus[i].online)
            continue;

        // Allocate per-CPU GDT/TSS.
        g_apTssSelector[i] = GdtInitAp(i);

        // Create per-CPU idle process.
        SchedulerInitApIdle(i);

        // Allocate per-CPU KernelCpuEnv + syscall stack.
        auto* env = static_cast<KernelCpuEnv*>(kmalloc(sizeof(KernelCpuEnv)));
        __builtin_memset(env, 0, sizeof(KernelCpuEnv));

        constexpr uint64_t SYSCALL_STACK_PAGES = 16;  // 64 KB
        constexpr uint64_t GUARD_PAGES = 1;
        VirtualAddress scBase = VmmAllocPages(
            SYSCALL_STACK_PAGES + GUARD_PAGES,
            VMM_WRITABLE, MemTag::KernelData, KernelPid);
        if (scBase)
        {
            VmmUnmapPage(KernelPageTable, scBase); // guard page
            uint64_t scTop = scBase.raw() + (SYSCALL_STACK_PAGES + GUARD_PAGES) * 0x1000 - 16;
            env->syscallStack = scTop;
        }
        env->syscallTable = SyscallGetTableAddress();
        env->selfPtr = reinterpret_cast<uint64_t>(env);

        g_apEnv[i] = env;
        SchedulerSetCpuEnv(i, env);

        KPrintf("SMP: CPU %u prepared (TSS sel=0x%x, env=%p)\n",
                i, g_apTssSelector[i], reinterpret_cast<void*>(env));
    }

    // Phase 2: activate all APs at once (no more BSP allocations needed).
    for (uint32_t i = 0; i < g_cpuCount && i < MAX_CPUS; ++i)
    {
        if (g_cpus[i].isBsp || !g_cpus[i].online)
            continue;
        __atomic_store_n(&g_apActivate[i], 1, __ATOMIC_RELEASE);
    }
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
    return 0;
}

} // namespace brook

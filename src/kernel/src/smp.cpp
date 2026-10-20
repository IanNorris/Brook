#include "smp_boot_policy.h"
#include "panic.h"
#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "serial.h"
#include "kprintf.h"
#include "scheduler.h"
#include "kvmclock.h"
#include "process.h"
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
#if BROOK_SMP_BOOT_TEST_STAGE
    volatile uint32_t testGate; // [+44] test-only stackless trampoline gate
#endif
} __attribute__((packed));

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static constexpr uint64_t AP_STACK_PAGES = 16;
static CpuInfo     g_cpus[MAX_CPUS];
static uint32_t    g_cpuCount    = 0;
static SmpBootAdmission g_bootAdmission;
static bool g_bootstrapRetained;
static void VerifyDenseOnlinePrefix();
#if BROOK_SMP_BOOT_TEST_STAGE
static volatile uint32_t g_smpTestPrivateGate;
static uint8_t g_smpTestTrampoline[4096];
#endif
static bool        g_smpReady    = false;

bool SmpBootstrapStackBounds(uint32_t cpu, uint64_t& base, uint64_t& end)
{
    if (cpu >= g_cpuCount || g_cpus[cpu].isBsp || !g_cpus[cpu].kernelStack)
        return false;
    // AP allocation reserves the final 16 bytes above its initial RSP.
    end = g_cpus[cpu].kernelStack + 16;
    base = end - AP_STACK_PAGES * 4096;
    return true;
}

// Slot-tagged acknowledgement, published only from the private high stack.
static volatile uint32_t g_apSignal = 0;

// ---------------------------------------------------------------------------
// Panic halt state — NMI handler captures per-CPU state here
// ---------------------------------------------------------------------------
// These are extern "C" so the naked asm NMI handler can reference them.
extern "C" volatile bool      g_panicHaltActive = false;
static CpuHaltedState         g_haltedState[MAX_CPUS] = {};
extern "C" volatile uint32_t  g_haltedCount = 0;

// Forward-declare ProcessCurrent for capturing PID
extern Process* ProcessCurrent();

// Forward-declare for NMI handler installation (defined below)
static void InstallPanicNmiHandler();

// Per-AP activation flag: BSP sets to 1 to tell AP to proceed.
static volatile uint32_t g_apActivate[MAX_CPUS] = {};
static bool g_apNmiReady[MAX_CPUS] = {};

// Count of APs that have completed GdtLoadOnAp (including ltr).
static volatile uint32_t g_apGdtReady = 0;

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

    // Match BSP: enable OSXSAVE + XCR0=x87|SSE|AVX so AVX-using user code
    // doesn't #UD on this AP.  Helper handles CPUID gating internally.
    CpuEnableXsaveAvx();
    CpuInitMemoryTypes();

    uint8_t myId = static_cast<uint8_t>(LapicRead(LapicReg::ID) >> 24);
    SerialPrintf("SMP: AP %u online\n", myId);

    // Determine our CPU index.
    uint32_t myCpuIndex = MAX_CPUS;
    for (uint32_t i = 0; i < g_cpuCount; ++i)
    {
        if (g_cpus[i].apicId == myId)
        {
            myCpuIndex = i;
            break;
        }
    }

    if (myCpuIndex==MAX_CPUS) {
        SerialPrintf("SMP: unknown APIC ID %u parked\n",myId);
        for (;;) __asm__ volatile("pause" ::: "memory");
    }
    uint64_t rsp,stackBase,stackEnd;
    __asm__ volatile("mov %%rsp,%0" : "=r"(rsp));
    if (!SmpBootstrapStackBounds(myCpuIndex,stackBase,stackEnd) ||
        rsp<stackBase || rsp>=stackEnd)
        KernelPanic("SMP: AP%u arrived on wrong bootstrap stack",myCpuIndex);
#if BROOK_SMP_BOOT_TEST_STAGE == 2
    if (myCpuIndex==3) {
        __atomic_store_n(&g_smpTestPrivateGate,2u,__ATOMIC_RELEASE);
        while (__atomic_load_n(&g_smpTestPrivateGate,__ATOMIC_ACQUIRE)!=3u)
            __asm__ volatile("pause" ::: "memory");
    }
#endif
#if BROOK_SMP_BOOT_TEST_STAGE
    if (myCpuIndex==3) {
        if (g_cpus[3].online || __atomic_load_n(&g_apActivate[3],__ATOMIC_ACQUIRE))
            KernelPanic("SMP_BOOT_TEST: late AP admitted");
        SerialPrintf("SMP_BOOT_TEST: late AP3 parked on private stack 0x%lx\n",rsp);
    }
#endif
    // All trampoline reads and temporary-stack operations have finished.
    __atomic_store_n(&g_apSignal,myCpuIndex+1,__ATOMIC_RELEASE);

    // Spin-wait for activation from BSP.
    while (!__atomic_load_n(&g_apActivate[myCpuIndex], __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");

    // ---- Per-CPU init ----

    // Load per-CPU GDT/TSS.
    GdtLoadOnAp(g_apTssSelector[myCpuIndex]);

    // Set up KernelCpuEnv and GS base for SWAPGS.
    KernelCpuEnv* env = g_apEnv[myCpuIndex];
    CpuSetKernelGsBase(env);
    // Kernel NMI delivery uses this CPU's TSS/IST and GS environment. A
    // timed-out, unactivated AP has neither and must never receive panic NMI.
    __atomic_store_n(&g_apNmiReady[myCpuIndex],true,__ATOMIC_SEQ_CST);
    // Either the panic scanner sees readiness, or this full StoreLoad pair
    // observes its already-published panic and prevents scheduler entry.
    if (__atomic_load_n(&g_panicHaltActive,__ATOMIC_SEQ_CST))
        for (;;) __asm__ volatile("cli; hlt" ::: "memory");
    __atomic_fetch_add(&g_apGdtReady,1,__ATOMIC_RELEASE);

    // Set up SYSCALL MSRs.
    CpuInitSyscallMsrs(reinterpret_cast<uint64_t>(&BrookSyscallDispatcher));

    if (KvmClockAvailable() && !KvmClockInitOnCpu(myCpuIndex))
        KernelPanic("SMP: pvclock registration failed before scheduler entry");
    KvmStealInit(myCpuIndex);

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

static bool BootAP(uint32_t slot,uint8_t apicId, uint8_t sipiVector)
{
    LapicWrite(0x280, 0);
    LapicRead(0x280);

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
    LapicWrite(LapicReg::ICR_LO, 0x00004500);
    if (!WaitForIpiIdle()) return false;

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
    LapicWrite(LapicReg::ICR_LO, 0x00008500);
    if (!WaitForIpiIdle()) return false;

    BusySleepMs(10);

    for (int i = 0; i < 2; ++i)
    {
        LapicWrite(0x280, 0);
        LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
        LapicWrite(LapicReg::ICR_LO, 0x00000600 | sipiVector);
        BusySleepUs(200);
        if (!WaitForIpiIdle()) return false;
    }

    for (uint32_t t = 0; t < 200; ++t)
    {
        if (__atomic_load_n(&g_apSignal, __ATOMIC_ACQUIRE)==slot+1)
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

    uint8_t ids[MAX_CPUS]{};
    const uint32_t count=madt.processorCount<=ACPI_MAX_PROCESSORS ? madt.processorCount : 0;
    g_cpuCount=SmpOrderApicIds(madt.apicIds,count,bspId,ids,MAX_CPUS);
    if (!g_cpuCount) KernelPanic("SMP: invalid CPU topology");
    for (uint32_t i=0;i<g_cpuCount;++i) {
        g_cpus[i].apicId=ids[i];
        g_cpus[i].isBsp=(i==0);
        g_cpus[i].online=(i==0);
    }
    if (g_cpuCount==1) {
        InstallPanicNmiHandler();
        return 1;
    }

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

        if (!g_bootAdmission.CanStart(i)) KernelPanic("SMP: invalid next CPU slot %u",i);
        uint8_t apicId = g_cpus[i].apicId;

        // This stack remains the AP's idle/scheduler stack after startup.
        // Scheduler and interrupt frames can exceed the old unguarded 16 KiB;
        // AP1 then spilled into the preceding framebuffer mapping. Match the
        // dedicated 64 KiB idle-stack capacity and reserve unmapped guards.
        VirtualAddress stackBase = VmmAllocKernelStack(AP_STACK_PAGES,
            MemTag::KernelData, KernelPid);
        if (!stackBase)
        {
            KPrintf("SMP: failed to allocate stack for AP %u\n", apicId);
            g_bootAdmission.Stop();
            break;
        }
        uint64_t stackTop = stackBase.raw() + AP_STACK_PAGES * 4096 - 16;
        g_cpus[i].kernelStack = stackTop;
        SerialPrintf("SMP: AP %u guarded stack 0x%lx..0x%lx (%lu KiB)\n",
                     i, stackBase.raw(), stackTop + 16, AP_STACK_PAGES * 4);

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

#if BROOK_SMP_BOOT_TEST_STAGE
        data->testGate=(BROOK_SMP_BOOT_TEST_STAGE==1 && i==3) ? 1u : 0u;
        for (uint32_t b=0;b<trampolineSize;++b) g_smpTestTrampoline[b]=trampolineDst[b];
#endif
        __asm__ volatile("wbinvd" ::: "memory");

        __atomic_store_n(&g_apSignal, 0, __ATOMIC_RELEASE);
        KPrintf("SMP: booting AP %u (LAPIC ID %u)...\n", i, apicId);

        if (BootAP(i,apicId, SIPI_VECTOR))
        {
            if (!g_bootAdmission.Accept(i,__atomic_load_n(&g_apSignal,__ATOMIC_ACQUIRE)))
                KernelPanic("SMP: non-prefix CPU admission at %u",i);
            g_cpus[i].online = true;
            KPrintf("SMP: AP %u (LAPIC ID %u) online ✓\n", i, apicId);
        }
        else
        {
            KPrintf("SMP: AP %u (LAPIC ID %u) failed to respond; remaining APs parked\n", i, apicId);
            // This AP may still consume low-memory code/data or its temporary
            // stack. Never rewrite/unmap them, start another AP, or free its
            // private stack after ambiguous startup completion.
#if BROOK_SMP_BOOT_TEST_STAGE
            if (i!=3 || data->stack64!=stackTop)
                KernelPanic("SMP_BOOT_TEST: unexpected startup failure");
            if (BROOK_SMP_BOOT_TEST_STAGE==1 && data->testGate!=2u)
                KernelPanic("SMP_BOOT_TEST: pre-stack gate was not reached");
            if (BROOK_SMP_BOOT_TEST_STAGE==2 &&
                __atomic_load_n(&g_smpTestPrivateGate,__ATOMIC_ACQUIRE)!=2u)
                KernelPanic("SMP_BOOT_TEST: private-stack gate was not reached");
#endif
            g_bootstrapRetained=true;
            g_bootAdmission.Stop();
            break;
        }
    }

    if (!g_bootstrapRetained) {
        VmmUnmapPage(VmmKernelCR3(), VirtualAddress(TRAMPOLINE_PHYS));
        VmmUnmapPage(VmmKernelCR3(), VirtualAddress(TEMP_STACK_PHYS));
    } else SerialPuts("SMP: retained bootstrap mappings for late AP; activation remains disabled\n");

#if BROOK_SMP_BOOT_TEST_STAGE
    if (!g_bootstrapRetained || g_bootAdmission.Count()!=3 || g_cpus[3].online)
        KernelPanic("SMP_BOOT_TEST: missing dense timeout prefix");
    auto* testData=reinterpret_cast<APDataBlock*>(trampolineDst+dataBlockOffset);
    // Real-mode startup patches its far-jump/GDT addresses in the code page.
    // Preserve the immutable input data block (apart from the explicit gate).
    for (uint32_t b=dataBlockOffset;b<dataBlockOffset+44;++b)
        if (trampolineDst[b]!=g_smpTestTrampoline[b])
            KernelPanic("SMP_BOOT_TEST: bootstrap input changed after timeout");
    if (BROOK_SMP_BOOT_TEST_STAGE==1) testData->testGate=3;
    else __atomic_store_n(&g_smpTestPrivateGate,3u,__ATOMIC_RELEASE);
    for (uint32_t t=0;t<1000 && __atomic_load_n(&g_apSignal,__ATOMIC_ACQUIRE)!=4u;++t)
        BusySleepMs(1);
    if (__atomic_load_n(&g_apSignal,__ATOMIC_ACQUIRE)!=4u)
        KernelPanic("SMP_BOOT_TEST: late AP did not finish private-stack handoff");
    SerialPrintf("SMP_BOOT_TEST_PASS stage=%u online=3 retained=1 late_slot=3\n",BROOK_SMP_BOOT_TEST_STAGE);
#endif
    VerifyDenseOnlinePrefix();
    KPrintf("SMP: %u/%u CPUs online\n", g_bootAdmission.Count(), g_cpuCount);

    InstallPanicNmiHandler();

    return g_bootAdmission.Count();
}

// ---------------------------------------------------------------------------
// SmpActivateAPs — set up per-CPU state and wake APs into scheduler
// ---------------------------------------------------------------------------

// Forward-declare the syscall table getter (defined in syscall.cpp).
uint64_t SyscallGetTableAddress();

static void VerifyDenseOnlinePrefix() {
    if (!g_cpuCount || !g_cpus[0].isBsp) KernelPanic("SMP: missing logical BSP");
    for (uint32_t i=0;i<g_cpuCount;++i)
        if (g_cpus[i].online!=(i<g_bootAdmission.Count()))
            KernelPanic("SMP: sparse online CPU set at %u",i);
}

void SmpPrepareAPs()
{
    VerifyDenseOnlinePrefix();
    if (g_bootAdmission.Count() <= 1)
        return;

    KPrintf("SMP: preparing %u APs for scheduling\n", g_bootAdmission.Count() - 1);

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
        if (!env) KernelPanic("SMP: OOM preparing KernelCpuEnv for AP%u",i);
        __builtin_memset(env, 0, sizeof(KernelCpuEnv));

        constexpr uint64_t SYSCALL_STACK_PAGES = 16;  // 64 KB
        constexpr uint64_t GUARD_PAGES = 1;
        VirtualAddress scBase = VmmAllocPages(
            SYSCALL_STACK_PAGES + GUARD_PAGES,
            VMM_WRITABLE, MemTag::KernelData, KernelPid);
        if (!scBase) KernelPanic("SMP: OOM preparing syscall stack for AP%u",i);
        {
            VmmUnmapPage(KernelPageTable, scBase); // guard page
            uint64_t scTop = scBase.raw() + (SYSCALL_STACK_PAGES + GUARD_PAGES) * 0x1000 - 16;
            env->syscallStack = scTop;
        }
        env->syscallTable = SyscallGetTableAddress();
        env->selfPtr = reinterpret_cast<uint64_t>(env);
        env->cpuIndex = i;

        g_apEnv[i] = env;
        SchedulerSetCpuEnv(i, env);

        KPrintf("SMP: CPU %u prepared (TSS sel=0x%x, env=%p)\n",
                i, g_apTssSelector[i], reinterpret_cast<void*>(env));
    }
}

void SmpActivateAPs()
{
    VerifyDenseOnlinePrefix();
    if (g_bootAdmission.Count() <= 1)
        return;

    // Signal all APs to start — they will init per-CPU state and enter
    // SchedulerStartAp which waits for g_schedulerRunning.
    uint32_t apCount = 0;
    for (uint32_t i = 0; i < g_cpuCount && i < MAX_CPUS; ++i)
    {
        if (g_cpus[i].isBsp || !g_cpus[i].online)
            continue;
        if (!g_apEnv[i] || !g_apEnv[i]->syscallStack || !g_apTssSelector[i])
            KernelPanic("SMP: incomplete preparation for AP%u",i);
        __atomic_store_n(&g_apActivate[i], 1, __ATOMIC_RELEASE);
        apCount++;
    }

    // Wait for all APs to complete GdtLoadOnAp (ltr modifies the GDT
    // by setting the TSS busy bit — must finish before write-protect).
    while (__atomic_load_n(&g_apGdtReady, __ATOMIC_ACQUIRE) < apCount)
        __asm__ volatile("pause" ::: "memory");
}

uint32_t SmpGetCpuCount()
{
    return g_bootAdmission.Count();
}

const CpuInfo* SmpGetCpu(uint32_t index)
{
    if (index >= g_cpuCount) return nullptr;
    return &g_cpus[index];
}

uint32_t SmpCurrentCpuIndex()
{
    // gives each CPU its own gs base).  Previously we did:
    //
    //     uint8_t myId = ApicGetId();
    //     for (i = 0; i < g_cpuCount; ++i)
    //         if (g_cpus[i].apicId == myId) return i;
    //
    // ApicGetId is correct at the *moment* it reads the LAPIC, but if a
    // timer interrupt migrates this thread between the read and the loop
    // (or anywhere inside the loop) the cached myId points at the wrong
    // CPU and we return a stale index — exactly the BRO-005 race that
    // produced ProcessCurrent() returning a Process for a different CPU's
    // thread.  Reading via gs is one instruction and immune to migration.
    //
    // Boot path: g_cpus[]/g_apEnv[] aren't populated until SmpInit runs.
    // Until then, only the BSP is online and gs may not yet be set up,
    // so the boot-time fast-path falls back to ApicGetId; SmpInit sets
    // env->cpuIndex during AP prepare and the BSP is always CPU 0.
    if (!g_smpReady)
    {
        uint8_t myId = ApicGetId();
        for (uint32_t i = 0; i < g_cpuCount; ++i)
            if (g_cpus[i].apicId == myId) return i;
        return 0;
    }
    uint64_t idx;
    __asm__ volatile("movq %%gs:176, %0" : "=r"(idx));
    return static_cast<uint32_t>(idx);
}

void SmpEnableFastCpuIndex()
{
    __atomic_store_n(&g_apNmiReady[0],true,__ATOMIC_RELEASE);
    g_smpReady = true;
}

void SmpSetCurrentCr3(uint32_t cpuIndex, uint64_t cr3)
{
    if (cpuIndex < g_cpuCount)
        __atomic_store_n(&g_cpus[cpuIndex].currentCr3, cr3, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// NMI handler for panic halt
// ---------------------------------------------------------------------------
// When g_panicHaltActive is true, the NMI handler captures the interrupted
// RIP/RSP/RBP into g_haltedState[thisCpu], then parks forever with cli;hlt
// (BRO-177: hlt, not pause — a halted AP must idle the core, not busy-spin).
// This is installed as the vector 2 handler during SmpInit. The captured live
// spin RIP is what PanicCaptureCpuList prefers (panic.cpp) — it reveals where
// each AP was ACTUALLY executing when the panic fired (e.g. spinning on a lock
// during the BRO-176 reap-stall hang), which the scheduler's stale savedCtx.rip
// cannot show for a Running process.
//
// We use a naked asm handler to avoid any compiler-generated prologue that
// might fault (e.g., SSE saves, red zone issues). On the panic path it never
// returns, so it clobbers registers freely. The CPU index is derived from RSP
// (the NMI runs on the per-CPU IST stack g_nmiStacks[idx], stride 4096) rather
// than GS, because the NMI may land while the interrupted code was in user mode
// with a user GS base — reading %gs:176 there would index with garbage.

// Base of g_nmiStacks[0][0]; set during InstallPanicNmiHandler. Referenced by
// the naked asm handler (RIP-relative) to convert RSP → CPU index.
extern "C" uint64_t g_nmiStackBase = 0;

// BRO-179: defined in apic.cpp. The naked NMI asm references these (RIP-relative
// flag check + call) on its non-panic path to service the active TLB epoch
// barrier for a CPU that is stuck in a long IF=0 region. Declared here so the
// inline-asm symbol references resolve at link time.
extern "C" volatile uint8_t g_tlbNmiActive;
extern "C" void TlbNmiFlushRecord(uint64_t cpuIdx);

// C recorder called from the naked NMI asm. Plain integer stores only (no locks,
// no SSE) so it is safe to call from NMI/IST context. Bounds-checked so a wild
// index can never fault the handler (which would nest a fault on the IST stack).
extern "C" void PanicNmiRecord(uint64_t cpuIdx, uint64_t rip,
                               uint64_t rsp, uint64_t rbp)
{
    if (cpuIdx >= MAX_CPUS) return;
    CpuHaltedState& hs = g_haltedState[cpuIdx];
    hs.rip    = rip;
    hs.rsp    = rsp;
    hs.rbp    = rbp;
    hs.pid    = 0;  // pid is resolved in panic.cpp from the scheduler's per-CPU table
    hs.halted = true;
}

__asm__(
    ".global PanicNmiHandlerAsm\n"
    ".type PanicNmiHandlerAsm,@function\n"
    "PanicNmiHandlerAsm:\n"
    "    cmpb $0, g_panicHaltActive(%rip)\n"
    "    jne .Lnmi_panic\n"
    // Not a panic NMI. Is the TLB epoch barrier escalating (BRO-179)? If so,
    // service this CPU's epoch flush; otherwise it is a spurious NMI — return.
    "    cmpb $0, g_tlbNmiActive(%rip)\n"
    "    jne .Lnmi_tlb\n"
    "    iretq\n"
    ".Lnmi_panic:\n"
    // --- panic path: never returns, free to clobber ---
    // CPU index = (RSP - g_nmiStackBase) >> 12  (IST NMI stack is per-CPU, 4096B).
    "    movq %rsp, %rax\n"
    "    subq g_nmiStackBase(%rip), %rax\n"
    "    shrq $12, %rax\n"
    // Gather the interrupted frame the CPU pushed: [rsp+0]=RIP, [rsp+24]=RSP.
    "    movq 0(%rsp), %rsi\n"          // arg2 = interrupted RIP
    "    movq 24(%rsp), %rdx\n"         // arg3 = interrupted RSP
    "    movq %rbp, %rcx\n"             // arg4 = RBP at interrupt
    "    movq %rax, %rdi\n"             // arg1 = cpu index
    "    andq $-16, %rsp\n"             // 16-byte align for the SysV call
    "    call PanicNmiRecord\n"
    "    lock incl g_haltedCount(%rip)\n"
    ".Lnmi_spin:\n"
    "    cli\n"
    "    hlt\n"
    "    jmp .Lnmi_spin\n"
    ".Lnmi_tlb:\n"
    // --- BRO-179 epoch-service path: RETURNS, so it MUST preserve the interrupted
    // context. NMI entry saves no GPRs, so save every caller-saved reg the C call
    // may clobber, plus rbp (used below as the stack-realign anchor). The handler
    // does only mov cr3 + an aligned atomic store to an always-mapped array; it
    // cannot fault, so it cannot prematurely unmask NMI / re-enter the IST stack.
    "    push %rax\n"
    "    push %rcx\n"
    "    push %rdx\n"
    "    push %rsi\n"
    "    push %rdi\n"
    "    push %r8\n"
    "    push %r9\n"
    "    push %r10\n"
    "    push %r11\n"
    "    push %rbp\n"
    // cpu index = ((entry RSP) - g_nmiStackBase) >> 12; entry RSP = current + 80
    // (10 pushes * 8). Derive from the IST RSP, NOT %gs (NMI may land on user GS).
    "    movq %rsp, %rax\n"
    "    addq $80, %rax\n"
    "    subq g_nmiStackBase(%rip), %rax\n"
    "    shrq $12, %rax\n"
    "    movq %rax, %rdi\n"             // arg1 = cpu index
    "    movq %rsp, %rbp\n"             // anchor original rsp
    "    andq $-16, %rsp\n"            // 16-byte align for the SysV call
    "    cld\n"
    "    call TlbNmiFlushRecord\n"
    "    movq %rbp, %rsp\n"             // restore pre-align rsp
    "    pop %rbp\n"
    "    pop %r11\n"
    "    pop %r10\n"
    "    pop %r9\n"
    "    pop %r8\n"
    "    pop %rdi\n"
    "    pop %rsi\n"
    "    pop %rdx\n"
    "    pop %rcx\n"
    "    pop %rax\n"
    "    iretq\n"
    ".size PanicNmiHandlerAsm,.-PanicNmiHandlerAsm\n"
);
extern "C" void PanicNmiHandlerAsm();

uint32_t SmpHaltAllAPs()
{
    // Only the first caller sends halt NMIs; subsequent calls observe its count.
    bool expected = false;
    if (!__atomic_compare_exchange_n(&g_panicHaltActive, &expected, true,
                                     false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        // Already in panic halt; do not send another round.
        return __atomic_load_n(&g_haltedCount, __ATOMIC_ACQUIRE);
    }

    __atomic_store_n(&g_haltedCount, 0, __ATOMIC_SEQ_CST);

    // A parked or not-yet-prepared AP has no valid NMI IST. Target only
    // accepted CPUs whose per-CPU TSS/GS setup is published, excluding self.
    if (!g_cpuCount) return 0; // panic before SMP/LAPIC discovery
    const uint8_t self=ApicGetId();
    uint32_t expected_count=0;
    for (uint32_t i=0;i<g_cpuCount;++i) {
        if (!g_cpus[i].online || g_cpus[i].apicId==self ||
            !__atomic_load_n(&g_apNmiReady[i],__ATOMIC_ACQUIRE)) continue;
        ++expected_count;ApicSendNmiBounded(g_cpus[i].apicId);
    }

    // Wait up to ~10ms for prepared targets to halt.
    for (uint32_t spin = 0; spin < 10000; ++spin)
    {
        if (__atomic_load_n(&g_haltedCount, __ATOMIC_ACQUIRE) >= expected_count)
            break;
        for (volatile int d = 0; d < 1000; d++) {}
    }

    return __atomic_load_n(&g_haltedCount, __ATOMIC_ACQUIRE);
}

bool SmpIsPanicActive()
{
    return __atomic_load_n(&g_panicHaltActive, __ATOMIC_ACQUIRE);
}

const CpuHaltedState* SmpGetHaltedState(uint32_t cpuIndex)
{
    if (cpuIndex >= MAX_CPUS) return nullptr;
    return &g_haltedState[cpuIndex];
}

// Install the panic-aware NMI handler. Called during SmpInit.
static void InstallPanicNmiHandler()
{
    // Cache the IST NMI-stack base for the asm handler's RSP→CPU-index math.
    // Idempotent (always the same address); safe to set on BSP and each AP.
    g_nmiStackBase = GdtNmiStacksBase();
    IdtInstallHandler(2, reinterpret_cast<void*>(PanicNmiHandlerAsm));
}

} // namespace brook

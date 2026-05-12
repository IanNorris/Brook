#include "apic.h"
#include "idt.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "serial.h"
#include "portio.h"
#include "smp.h"
#include "spinlock.h"

// Declared at global scope in idt.cpp
void IdtInstallHandler(uint8_t vector, void* handler);

namespace brook {

// Per-CPU lock diagnostics — indexed by CPU index.
LockDiagInfo g_lockDiag[MAX_CPUS] = {};

// ---------------------------------------------------------------------------
// Port I/O
// ---------------------------------------------------------------------------

// Short I/O delay using an unused port.
static inline void IoDelay()
{
    outb(0x80, 0);
}

// ---------------------------------------------------------------------------
// MSR access
// ---------------------------------------------------------------------------

static inline uint64_t ReadMsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

static inline void WriteMsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"(static_cast<uint32_t>(val)),
                     "d"(static_cast<uint32_t>(val >> 32)));
}

// ---------------------------------------------------------------------------
// LAPIC MMIO access
// ---------------------------------------------------------------------------

static uint64_t g_lapicVirt = 0;
static uint32_t g_timerTicksPerMs = 0;
static uint32_t g_timerTicksOrigCalibration = 0;  // snapshot of initial calibration for clamp

// Raw ISR invocation counter, only ever incremented by the BSP LAPIC timer
// ISR. Never adjusted by RTC nudge/slew code, so it reflects actual LAPIC
// firing rate and can be used as drift signal for rate correction.
volatile uint64_t g_lapicRawTickCount = 0;

// Epoch incremented whenever the rate changes. Each CPU's LAPIC timer ISR
// compares against its cached last-seen epoch and reprograms its local
// TIMER_INIT_CNT from g_timerTicksPerMs when stale. Avoids needing IPIs.
static volatile uint32_t g_timerRateEpoch = 0;
static constexpr uint32_t MAX_CPUS_FOR_RATE_CACHE = 32;
static volatile uint32_t g_lastSeenRateEpoch[MAX_CPUS_FOR_RATE_CACHE] = {};

static inline uint32_t LapicRead(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t*>(g_lapicVirt + offset);
}

static inline void LapicWrite(uint32_t offset, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t*>(g_lapicVirt + offset) = val;
}

// ---------------------------------------------------------------------------
// Step 1: Disable the legacy 8259 PIC
// ---------------------------------------------------------------------------

static void DisablePic()
{
    // Send ICW1 to both PICs.
    outb(0x20, 0x11);  IoDelay();
    outb(0xA0, 0x11);  IoDelay();
    // ICW2: remap master → vectors 32-39, slave → vectors 40-47.
    outb(0x21, 0x20);  IoDelay();
    outb(0xA1, 0x28);  IoDelay();
    // ICW3: cascade.
    outb(0x21, 0x04);  IoDelay();
    outb(0xA1, 0x02);  IoDelay();
    // ICW4: 8086 mode.
    outb(0x21, 0x01);  IoDelay();
    outb(0xA1, 0x01);  IoDelay();
    // Mask ALL interrupts on both PICs.
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    SerialPuts("APIC: 8259 PIC disabled\n");
}

// ---------------------------------------------------------------------------
// Step 2: Enable LAPIC via IA32_APIC_BASE MSR
// ---------------------------------------------------------------------------

static constexpr uint32_t IA32_APIC_BASE_MSR   = 0x1B;
static constexpr uint64_t APIC_BASE_GLOBAL_EN  = (1ULL << 11);

static bool EnableLapicMsr()
{
    uint64_t base = ReadMsr(IA32_APIC_BASE_MSR);
    base |= APIC_BASE_GLOBAL_EN;
    WriteMsr(IA32_APIC_BASE_MSR, base);

    // Verify it's still enabled.
    uint64_t verify = ReadMsr(IA32_APIC_BASE_MSR);
    if (!(verify & APIC_BASE_GLOBAL_EN))
    {
        SerialPuts("APIC: failed to enable via MSR\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Step 3: Map LAPIC MMIO into virtual address space
// ---------------------------------------------------------------------------

static bool MapLapic(uint64_t physBase)
{
    // LAPIC is a single 4KB page.
    g_lapicVirt = VmmAllocPages(1, VMM_WRITABLE, MemTag::Device, KernelPid).raw();
    if (g_lapicVirt == 0)
    {
        SerialPuts("APIC: failed to allocate virtual page for LAPIC\n");
        return false;
    }

    // Remap the allocated virtual page to the LAPIC physical page.
    // VmmAllocPages already mapped a physical page there — unmap it first,
    // free the backing page, then map LAPIC physical.
    PhysicalAddress oldPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(g_lapicVirt));
    VmmUnmapPage(KernelPageTable, VirtualAddress(g_lapicVirt));
    if (oldPhys) PmmFreePage(oldPhys);

    if (!VmmMapPage(KernelPageTable, VirtualAddress(g_lapicVirt), PhysicalAddress(physBase),
                    VMM_WRITABLE | VMM_NO_EXEC,
                    MemTag::Device, KernelPid))
    {
        SerialPuts("APIC: failed to map LAPIC MMIO\n");
        return false;
    }

    SerialPrintf("APIC: LAPIC mapped phys 0x%p → virt 0x%p\n",
                 reinterpret_cast<void*>(physBase),
                 reinterpret_cast<void*>(g_lapicVirt));
    return true;
}

// ---------------------------------------------------------------------------
// Step 4: Software-enable the LAPIC and set spurious vector
// ---------------------------------------------------------------------------

static void SoftEnableLapic()
{
    // Set spurious interrupt vector and enable bit.
    LapicWrite(LapicReg::SVR,
               LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    // Zero the Task Priority Register — accept all interrupt classes.
    LapicWrite(LapicReg::TPR, 0);
}

// ---------------------------------------------------------------------------
// LAPIC timer spurious ISR — just send EOI.
// ---------------------------------------------------------------------------

// Global tick counter (incremented every ~1ms by LAPIC timer).
// Used by syscall timing (clock_gettime, nanosleep).
volatile uint64_t g_lapicTickCount = 0;

// Forward-declare scheduler tick (defined in scheduler.cpp).
void SchedulerTimerTick(bool allowPreempt);

// Forward-declare thread state dump (defined in scheduler.cpp).
void SchedulerDumpThreadStates();

// Forward-declare profiler sample (defined in profiler.cpp).
void ProfilerSample(uint64_t interruptedRip, uint64_t interruptedCs, uint64_t interruptedRbp);

// Forward-declare RTC recalibration (defined in rtc.cpp).
void RtcRecalibrateLapic();

// C handler called from the naked ISR wrapper below.
// interruptedRip/interruptedCs/interruptedRbp are passed from the naked
// handler (extracted from the CPU interrupt frame on the stack).
static void LapicTimerHandlerInner(uint64_t interruptedRip, uint64_t interruptedCs,
                                    uint64_t interruptedRbp)
{
    LapicWrite(LapicReg::EOI, 0);

    // Only BSP maintains the global tick and composites framebuffers.
    // Using LAPIC ID check (cheaper than SmpCurrentCpuIndex).
    uint8_t cpuId = static_cast<uint8_t>(LapicRead(LapicReg::ID) >> 24);
    if (cpuId == 0)
    {
        g_lapicTickCount++;
        g_lapicRawTickCount++;

        // Re-check CMOS every ~1024 ticks (~1 second) to correct for
        // LAPIC calibration drift under host turbo / KVM dilation.
        if ((g_lapicTickCount & 0x3FF) == 0)
        {
            RtcRecalibrateLapic();
        }
    }

    // Propagate rate changes to every CPU: when BSP adjusts the timer rate,
    // it bumps g_timerRateEpoch. Each CPU checks here and reprograms its
    // local LAPIC TIMER_INIT_CNT when stale. Writing TIMER_INIT_CNT during
    // a running periodic timer is safe — it restarts the countdown.
    if (cpuId < MAX_CPUS_FOR_RATE_CACHE)
    {
        uint32_t curEpoch = g_timerRateEpoch;
        if (g_lastSeenRateEpoch[cpuId] != curEpoch)
        {
            LapicWrite(LapicReg::TIMER_INIT_CNT, g_timerTicksPerMs);
            g_lastSeenRateEpoch[cpuId] = curEpoch;
        }
    }

    // Record a profiler sample (fast no-op when profiling is disabled).
    ProfilerSample(interruptedRip, interruptedCs, interruptedRbp);

    // Drive scheduler wakeups/accounting on every CPU. Only preempt arbitrary
    // non-idle work when the interrupted context was user mode; Brook kernel
    // code is not generally safe to timeslice while holding internal locks.
    bool userMode = (interruptedCs & 3) != 0;
    SchedulerTimerTick(userMode);
}

// Naked ISR entry for LAPIC timer (vector 32).
// Performs swapgs when entering from ring 3 so that gs-relative kernel data
// (KernelCpuEnv at gs:0) is accessible throughout the handler.
// Passes the interrupted RIP (rdi) and CS (rsi) to the inner handler for
// profiler sampling.
//
// IMPORTANT: apic.cpp/scheduler.cpp/profiler.cpp are compiled with
// -mgeneral-regs-only, so this interrupt path must not touch vector state.
// The scheduler saves/restores user x87/SSE/AVX state only at actual context
// switches.
__attribute__((naked))
static void LapicTimerHandler(void)
{
    __asm__ volatile(
        // If interrupted from ring 3, CS (at RSP+8) has CPL bits set.
        "testq $3, 8(%%rsp)\n\t"
        "jz 1f\n\t"
        "swapgs\n\t"
        "1:\n\t"

        // Save all GPRs. This ISR can preempt kernel C code and then context
        // switch before returning; callee-saved registers live in the
        // interrupted frame must survive that whole path.
        "push %%rax\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%rbp\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"

        // Stack layout after 15 pushes:
        //   RSP+64  = interrupted RBP
        //   RSP+120 = interrupted RIP
        //   RSP+128 = interrupted CS
        "movq %%rsp, %%rax\n\t"
        "movq 120(%%rax), %%rdi\n\t"    // arg1 = interrupted RIP
        "movq 128(%%rax), %%rsi\n\t"    // arg2 = interrupted CS
        "movq 64(%%rax), %%rdx\n\t"     // arg3 = interrupted RBP

        "cld\n\t"
        "call %P0\n\t"

        // Restore GPRs
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"

        // If returning to ring 3, swap gs back.
        "testq $3, 8(%%rsp)\n\t"
        "jz 2f\n\t"
        "swapgs\n\t"
        "2:\n\t"
        "iretq\n\t"
        :
        : "i"(LapicTimerHandlerInner)
        : "memory"
    );
}

// ---------------------------------------------------------------------------
// Step 5: Calibrate LAPIC timer against PIT channel 2 (~10ms)
//
// PIT channel 2 is connected to the PC speaker gate but can be used as a
// one-shot timer without enabling the speaker.  We gate it for ~10ms and
// count how many LAPIC timer ticks that takes.
// ---------------------------------------------------------------------------

static constexpr uint32_t PIT_FREQUENCY      = 1193182;   // Hz

static uint32_t CalibrateLapicTimerOnce(uint32_t windowMs)
{
    LapicWrite(LapicReg::TIMER_DIVIDE, 0x3);

    uint32_t pitTicks = (PIT_FREQUENCY * windowMs) / 1000;

    uint8_t prev61 = inb(0x61);
    outb(0x61, (prev61 & ~0x02) | 0x01);

    outb(0x43, 0xB0);
    outb(0x42, static_cast<uint8_t>(pitTicks & 0xFF));
    outb(0x42, static_cast<uint8_t>(pitTicks >> 8));

    LapicWrite(LapicReg::TIMER_INIT_CNT, 0xFFFFFFFF);

    while (!(inb(0x61) & 0x20)) {}

    uint32_t remaining = LapicRead(LapicReg::TIMER_CUR_CNT);
    LapicWrite(LapicReg::TIMER_INIT_CNT, 0);

    outb(0x61, prev61);

    uint32_t elapsed = 0xFFFFFFFF - remaining;
    return elapsed / windowMs;
}

static uint32_t CalibrateLapicTimer()
{
    // Take the max of several samples. CPU steal (host load, turbo transitions)
    // only makes a sample read LOW — the LAPIC still advances at a fixed
    // hardware rate, but we observe fewer cycles per PIT window. So the
    // maximum of several samples is the closest to the true rate.
    //
    // Also use a longer per-sample window (25ms) to reduce the relative
    // impact of any single stall.
    uint32_t best = 0;
    uint32_t samples[5] = {};
    for (int i = 0; i < 5; ++i)
    {
        uint32_t s = CalibrateLapicTimerOnce(25);
        samples[i] = s;
        if (s > best) best = s;
    }

    SerialPrintf("APIC: LAPIC calibrated %u ticks/ms (samples %u %u %u %u %u)\n",
                 best, samples[0], samples[1], samples[2], samples[3], samples[4]);
    return best;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ApicInit(uint64_t localApicPhysical)
{
    if (localApicPhysical == 0)
    {
        SerialPuts("APIC: no LAPIC address\n");
        return false;
    }

    DisablePic();

    if (!EnableLapicMsr())   return false;
    if (!MapLapic(localApicPhysical)) return false;

    SoftEnableLapic();

    // Install LAPIC timer handler at vector 32 (replaces PIC IRQ0 stub).
    IdtInstallHandler(LAPIC_TIMER_VECTOR,
                      reinterpret_cast<void*>(LapicTimerHandler));

    g_timerTicksPerMs = CalibrateLapicTimer();
    g_timerTicksOrigCalibration = g_timerTicksPerMs;

    // Program the LAPIC timer: periodic, ~1ms interval.
    LapicWrite(LapicReg::TIMER_DIVIDE, 0x3);  // divide by 16
    LapicWrite(LapicReg::LVT_TIMER,
               LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    LapicWrite(LapicReg::TIMER_INIT_CNT, g_timerTicksPerMs);  // 1ms period

    SerialPrintf("APIC: LAPIC ID=%u, version=0x%x, timer running at 1ms intervals\n",
                 ApicGetId(),
                 LapicRead(LapicReg::VERSION) & 0xFF);

    // Sanity-check: verify LAPIC-tick-counted ms advance at the same rate as
    // an independent PIT-gated wall-clock window.  PIT channel 2 is 16-bit,
    // so a single one-shot tops out at ~54ms (65535/1193182).  For longer
    // windows we chain multiple 50ms one-shots.  Expected: dMs ~= windowMs.
    {
        __asm__ volatile("sti");

        auto measure = [](uint32_t windowMs) {
            constexpr uint32_t CHUNK_MS = 50;
            constexpr uint32_t PIT_CHUNK = (PIT_FREQUENCY * CHUNK_MS) / 1000;

            uint64_t tStart = g_lapicTickCount;
            uint32_t chunks = windowMs / CHUNK_MS;
            uint8_t  prev61 = inb(0x61);
            for (uint32_t i = 0; i < chunks; i++)
            {
                outb(0x61, prev61 & ~0x03);
                outb(0x43, 0xB0);
                outb(0x42, static_cast<uint8_t>(PIT_CHUNK & 0xFF));
                outb(0x42, static_cast<uint8_t>(PIT_CHUNK >> 8));
                outb(0x61, (prev61 & ~0x02) | 0x01);
                while (!(inb(0x61) & 0x20)) {}
            }
            outb(0x61, prev61);
            uint64_t tEnd = g_lapicTickCount;

            uint64_t dMs   = tEnd - tStart;
            uint32_t realMs = chunks * CHUNK_MS;
            int32_t  skew  = static_cast<int32_t>(dMs) -
                             static_cast<int32_t>(realMs);
            SerialPrintf("APIC: self-test — %u ms real, %lu ms kernel "
                         "(skew %d ms, %d%%)\n",
                         realMs, dMs, skew,
                         (skew * 100) / static_cast<int32_t>(realMs));
        };

        measure(50);
        measure(200);
        measure(500);
    }

    return true;
}

void ApicSendEoi()
{
    LapicWrite(LapicReg::EOI, 0);
}

uint8_t ApicGetId()
{
    return static_cast<uint8_t>(LapicRead(LapicReg::ID) >> 24);
}

uint32_t ApicGetTimerTicksPerMs()
{
    return g_timerTicksPerMs;
}

// Adjust the LAPIC periodic timer rate based on observed drift vs RTC.
// `observedTicksPerSec` is how many ISR-increments actually occurred in the
// last real wall-clock second (measured from CMOS).  If the LAPIC is firing
// too fast, observed > 1000 and we increase INIT_COUNT so the next interval
// is longer (fewer ticks/sec).  Proportional control with 50% damping and a
// 2% deadband to avoid oscillation.  Clamped to [0.25×, 4×] the original
// boot-time calibration so we can never brick the scheduler tick.
void ApicAdjustTimerRate(uint32_t observedTicksPerSec)
{
    if (g_timerTicksPerMs == 0 || g_timerTicksOrigCalibration == 0) return;

    // Reject wildly implausible observations — likely a missed RTC second
    // boundary or CPU halt during the measurement window.
    if (observedTicksPerSec < 200 || observedTicksPerSec > 5000) return;

    // Deadband: ignore errors < 10%.  CMOS 1s resolution + integer sampling
    // windows adds a few % phase noise even at 10s windows; we'd rather
    // accept 10% wall-clock drift than oscillate.
    int32_t errorTicks = static_cast<int32_t>(observedTicksPerSec) - 1000;
    if (errorTicks > -100 && errorTicks < 100) return;

    // Observed rate too high → LAPIC fires too often → increase INIT_COUNT.
    // 25% proportional, 75% carry-over (heavy damping for stability).
    uint64_t cur = g_timerTicksPerMs;
    uint64_t proposed = (cur * observedTicksPerSec + 500) / 1000;
    uint64_t damped = (cur * 3 + proposed) / 4;

    uint64_t lo = g_timerTicksOrigCalibration / 2;
    uint64_t hi = static_cast<uint64_t>(g_timerTicksOrigCalibration) * 2;
    if (damped < lo) damped = lo;
    if (damped > hi) damped = hi;

    if (damped == cur) return;

    g_timerTicksPerMs = static_cast<uint32_t>(damped);
    LapicWrite(LapicReg::TIMER_INIT_CNT, g_timerTicksPerMs);
    // Bump epoch so APs pick up the new rate in their next LAPIC timer ISR.
    __atomic_add_fetch(&g_timerRateEpoch, 1, __ATOMIC_RELEASE);

    SerialPrintf("apic: rate adjust obs=%u/s old=%u new=%u\n",
                 observedTicksPerSec, static_cast<uint32_t>(cur),
                 g_timerTicksPerMs);
}

uint64_t ApicGetLapicVirtBase()
{
    return g_lapicVirt;
}

// ---------------------------------------------------------------------------
// NMI delivery via LAPIC ICR (for panic halt)
// ---------------------------------------------------------------------------
// ICR format (low 32 bits):
//   bits  7:0  = vector (ignored for NMI delivery mode)
//   bits 10:8  = delivery mode (100 = NMI)
//   bit  11    = destination mode (0 = physical)
//   bit  14    = level (1 = assert)
//   bits 19:18 = destination shorthand (00 = use ICR_HI dest field)
// ICR_HI bits 31:24 = destination APIC ID

void ApicSendNmi(uint8_t targetApicId)
{
    // Wait for previous IPI to be delivered
    while (LapicRead(LapicReg::ICR_LO) & (1u << 12))
        __asm__ volatile("pause");

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(targetApicId) << 24);
    // Delivery mode = NMI (0b100 << 8), level assert (1 << 14)
    LapicWrite(LapicReg::ICR_LO, (0x4 << 8) | (1u << 14));
}

void ApicBroadcastNmi()
{
    if (!g_lapicVirt) return; // LAPIC not initialized

    // Wait for previous IPI to be delivered (with bounded timeout)
    for (int i = 0; i < 100000; i++) {
        if (!(LapicRead(LapicReg::ICR_LO) & (1u << 12)))
            break;
        __asm__ volatile("pause");
    }

    // Shorthand = 11 (all excluding self), delivery mode = NMI
    LapicWrite(LapicReg::ICR_LO, (0x3 << 18) | (0x4 << 8) | (1u << 14));
}

// ---------------------------------------------------------------------------
// Reschedule IPI — kicks idle CPUs into running the scheduler
// ---------------------------------------------------------------------------

// The handler needs the same naked ISR treatment as the timer handler
// because SchedulerTimerTick can context-switch, which requires all GPRs
// saved and swapgs handled properly.
static void ReschedIpiHandlerInner(uint64_t interruptedCs)
{
    LapicWrite(LapicReg::EOI, 0);
    // Only allow preemption when the IPI interrupted user mode (CPL 3).
    // Brook kernel code is not preemptible — it may hold internal locks
    // (KMutex, KRwLock, ticket locks) that would deadlock or corrupt
    // callee-saved register state if the process were descheduled mid-hold.
    bool userMode = (interruptedCs & 3) != 0;
    SchedulerTimerTick(userMode);
}

__attribute__((naked))
static void ReschedIpiHandler(void)
{
    __asm__ volatile(
        "testq $3, 8(%%rsp)\n\t"
        "jz 1f\n\t"
        "swapgs\n\t"
        "1:\n\t"
        "push %%rax\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%rbp\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"

        // Extract interrupted CS from the interrupt frame.
        // After 15 GPR pushes the interrupt frame CS is at RSP + 128.
        "movq 128(%%rsp), %%rdi\n\t"    // arg1 = interrupted CS

        "cld\n\t"
        "call %P0\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "testq $3, 8(%%rsp)\n\t"
        "jz 2f\n\t"
        "swapgs\n\t"
        "2:\n\t"
        "iretq\n\t"
        :
        : "i"(ReschedIpiHandlerInner)
        : "memory"
    );
}

void ApicSendRescheduleIpi(uint8_t targetApicId)
{
    // Wait for previous IPI to be delivered
    while (LapicRead(LapicReg::ICR_LO) & (1u << 12))
        __asm__ volatile("pause");

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(targetApicId) << 24);
    // Delivery mode = fixed (0), level assert, vector = LAPIC_RESCHED_VECTOR
    LapicWrite(LapicReg::ICR_LO, LAPIC_RESCHED_VECTOR | (1u << 14));
}

void ApicInitReschedIpi()
{
    ::IdtInstallHandler(LAPIC_RESCHED_VECTOR,
                        reinterpret_cast<void*>(ReschedIpiHandler));
}

// ---------------------------------------------------------------------------
// TLB Shootdown IPI — with deadlock detection diagnostics
// ---------------------------------------------------------------------------
//
// Protocol:
//   1. Initiator modifies PTE, does local invlpg
//   2. Initiator acquires g_tlbRequest.lock (IF=0)
//   3. Fills targetCr3, addr (0 = full flush), sets pendingCount
//   4. Sends TLB_SHOOTDOWN_VECTOR IPI to each target CPU
//   5. Spins waiting for pendingCount to reach 0, with TIMEOUT
//   6. Releases lock
//
// Handler (naked ISR on target CPU):
//   1. Compare current CR3 with g_tlbRequest.targetCr3
//   2. If match: invlpg(addr) or CR3 reload (addr == 0)
//   3. Decrement pendingCount
//   4. EOI
//
// The handler must NOT acquire any lock — the initiator holds the spinlock
// and waits synchronously, so any lock attempt would deadlock.

struct TlbShootdownRequest {
    IrqSpinLock     lock;             // Must be IrqSpinLock: initiator needs IF=0 during wait
    volatile uint64_t pendingCount;   // decremented by each responder
    uint64_t        targetCr3;        // only invalidate if CPU's CR3 matches
    uint64_t        addr;             // page VA to invalidate (0 = full flush)
    volatile uint64_t ackBitmap;      // bit set by each CPU on ack (diagnostic)
};

static TlbShootdownRequest g_tlbRequest;

// Timeout for the spin-wait: ~10ms at 2.5GHz ≈ 25M iterations of pause loop.
// Each pause is ~10-100 cycles, so 500K iterations ≈ 5-50ms.
static constexpr uint64_t TLB_SHOOTDOWN_TIMEOUT = 10000000; // ~10ms at 1GHz loop

static void TlbShootdownHandlerInner()
{
    uint64_t myCr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(myCr3));

    if ((myCr3 & ~0xFFFULL) == (g_tlbRequest.targetCr3 & ~0xFFFULL))
    {
        if (g_tlbRequest.addr != 0)
        {
            // Single-page invalidation
            __asm__ volatile("invlpg (%0)" :: "r"(g_tlbRequest.addr) : "memory");
        }
        else
        {
            // Full flush: reload CR3
            __asm__ volatile("movq %0, %%cr3" :: "r"(myCr3) : "memory");
        }
    }

    // Mark this CPU as acked (diagnostic bitmap).
    uint32_t cpu;
    __asm__ volatile("movl %%gs:176, %0" : "=r"(cpu));
    if (cpu < 64)
        __atomic_fetch_or(&g_tlbRequest.ackBitmap, 1ULL << cpu, __ATOMIC_RELAXED);

    __atomic_fetch_sub(&g_tlbRequest.pendingCount, 1, __ATOMIC_ACQ_REL);

    LapicWrite(LapicReg::EOI, 0);
}

// Naked ISR — same structure as ReschedIpiHandler.
__attribute__((naked))
static void TlbShootdownHandler(void)
{
    __asm__ volatile(
        // swapgs if we interrupted user mode
        "testq $3, 8(%%rsp)\n\t"
        "jz 1f\n\t"
        "swapgs\n\t"
        "1:\n\t"
        "push %%rax\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%rbp\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "cld\n\t"
        "call %P0\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "testq $3, 8(%%rsp)\n\t"
        "jz 2f\n\t"
        "swapgs\n\t"
        "2:\n\t"
        "iretq\n\t"
        :
        : "i"(TlbShootdownHandlerInner)
        : "memory"
    );
}

void ApicInitTlbShootdown()
{
    ::IdtInstallHandler(TLB_SHOOTDOWN_VECTOR,
                        reinterpret_cast<void*>(TlbShootdownHandler));
}

// Send a TLB shootdown IPI to a specific CPU by its CPU index.
static void ApicSendTlbShootdownIpi(uint32_t targetCpuIndex)
{
    const CpuInfo* info = SmpGetCpu(targetCpuIndex);
    if (!info || !info->online)
        return;

    uint8_t apicId = info->apicId;

    // Wait for previous IPI delivery to complete
    while (LapicRead(LapicReg::ICR_LO) & (1u << 12))
        __asm__ volatile("pause");

    LapicWrite(LapicReg::ICR_HI, static_cast<uint32_t>(apicId) << 24);
    // Fixed delivery, level assert, vector = TLB_SHOOTDOWN_VECTOR
    LapicWrite(LapicReg::ICR_LO, TLB_SHOOTDOWN_VECTOR | (1u << 14));
}

// Dump diagnostic info when TLB shootdown times out — called with IF=0.
// Uses SerialPrintf which busy-waits the UART (safe without locks).
// Retained for future use if graceful forgiveness is insufficient.
[[noreturn, maybe_unused]]
static void TlbShootdownTimeoutPanic(uint32_t myCpu, uint64_t targetMask,
                                      uint64_t ackBitmap, uint64_t targetCr3,
                                      uint64_t addr)
{
    uint64_t pending = __atomic_load_n(&g_tlbRequest.pendingCount, __ATOMIC_ACQUIRE);
    uint64_t notAcked = targetMask & ~ackBitmap;

    SerialPrintf("\n!!! TLB_SHOOTDOWN: TIMEOUT — deadlock detected !!!\n");
    SerialPrintf("  initiator: CPU %u\n", myCpu);
    SerialPrintf("  targetCr3: 0x%lx  addr: 0x%lx\n", targetCr3, addr);
    SerialPrintf("  pendingCount: %lu\n", pending);
    SerialPrintf("  targetMask: 0x%lx  ackBitmap: 0x%lx\n", targetMask, ackBitmap);
    SerialPrintf("  not-acked CPUs (IF=0, holding lock?):\n");

    uint32_t cpuCount = SmpGetCpuCount();
    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (!(notAcked & (1ULL << i))) continue;

        const auto& diag = g_lockDiag[i];
        if (diag.held && diag.file)
        {
            SerialPrintf("    CPU %u: IF=0, lock at %s:%u\n",
                         i, diag.file, diag.line);
        }
        else
        {
            SerialPrintf("    CPU %u: IF=0, no lock info (held=%u)\n",
                         i, diag.held);
        }
    }

    SerialPrintf("  acked CPUs:\n");
    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (!(ackBitmap & (1ULL << i))) continue;
        SerialPrintf("    CPU %u: acked OK\n", i);
    }

    // Halt all APs and panic
    SerialPrintf("KERNEL PANIC: TLB shootdown deadlock\n");
    SmpHaltAllAPs();
    while (true) __asm__ volatile("hlt");
}

// Common spin-wait with timeout and graceful retry.
// If a CPU doesn't ack within the timeout, forgive it — but ONLY if it's
// not running the target CR3. A CPU on a different CR3 will flush its TLB
// naturally when it switches back. A CPU on the target CR3 with stale
// entries is dangerous (e.g., stale writable entry after CoW fork), so
// we keep waiting for those.
static void TlbShootdownWait(uint32_t myCpu, uint64_t targetMask,
                              uint64_t targetCr3, uint64_t addr)
{
    uint64_t spins = 0;
    while (__atomic_load_n(&g_tlbRequest.pendingCount, __ATOMIC_ACQUIRE) != 0)
    {
        __asm__ volatile("pause" ::: "memory");
        if (++spins > TLB_SHOOTDOWN_TIMEOUT)
        {
            uint64_t ackBitmap = __atomic_load_n(&g_tlbRequest.ackBitmap, __ATOMIC_ACQUIRE);
            uint64_t notAcked = targetMask & ~ackBitmap;

            // Only forgive CPUs that are NOT currently running the target
            // CR3. Those CPUs will flush their TLB on the next context
            // switch (CR3 reload). CPUs still on the target CR3 must ack
            // to guarantee no stale writable entries remain (critical for
            // CoW fork correctness).
            uint32_t forgiven = 0;
            uint32_t stillWaiting = 0;
            uint32_t cpuCount = SmpGetCpuCount();
            for (uint32_t i = 0; i < cpuCount; i++)
            {
                if (!(notAcked & (1ULL << i))) continue;

                // Read the CPU's current CR3 from per-CPU data.
                const CpuInfo* info = SmpGetCpu(i);
                uint64_t cpuCr3 = info ? __atomic_load_n(&info->currentCr3, __ATOMIC_ACQUIRE) : 0;
                bool onTargetCr3 = cpuCr3 && ((cpuCr3 & ~0xFFFULL) == (targetCr3 & ~0xFFFULL));

                if (!onTargetCr3)
                {
                    __atomic_fetch_sub(&g_tlbRequest.pendingCount, 1, __ATOMIC_ACQ_REL);
                    __atomic_fetch_or(&g_tlbRequest.ackBitmap, 1ULL << i, __ATOMIC_RELAXED);
                    forgiven++;
                }
                else
                {
                    stillWaiting++;
                }
            }

            if (forgiven > 0 && stillWaiting == 0)
            {
                SerialPrintf("TLB_SHOOTDOWN: forgave %u CPU(s) (different CR3), cr3=0x%lx addr=0x%lx\n",
                             forgiven, targetCr3, addr);
                break;
            }
            else if (forgiven > 0 || stillWaiting > 0)
            {
                SerialPrintf("TLB_SHOOTDOWN: forgave %u, waiting for %u on target CR3=0x%lx\n",
                             forgiven, stillWaiting, targetCr3);
                // Reset spin counter to keep waiting for CPUs on target CR3
                spins = 0;
            }
            else
            {
                break;
            }
        }
    }
}

void TlbShootdown(uint64_t targetCr3, uint64_t virtualAddr, uint64_t cpuMask)
{
    if (SmpGetCpuCount() <= 1)
        return;

    // Local invalidation first
    __asm__ volatile("invlpg (%0)" :: "r"(virtualAddr) : "memory");

    uint32_t myCpu = SmpCurrentCpuIndex();
    uint32_t cpuCount = SmpGetCpuCount();

    // Build target mask: intersect cpuMask with online CPUs, exclude self
    uint64_t targetMask = 0;
    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (i == myCpu) continue;
        if (!(cpuMask & (1ULL << i))) continue;
        const CpuInfo* info = SmpGetCpu(i);
        if (info && info->online)
            targetMask |= (1ULL << i);
    }

    if (targetMask == 0)
        return;

    uint32_t targetCount = __builtin_popcountll(targetMask);

    uint64_t flags = IrqSpinLockAcquire(&g_tlbRequest.lock);

    g_tlbRequest.targetCr3 = targetCr3;
    g_tlbRequest.addr      = virtualAddr;
    __atomic_store_n(&g_tlbRequest.ackBitmap, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tlbRequest.pendingCount, targetCount, __ATOMIC_RELEASE);

    // Send IPI to each target
    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (targetMask & (1ULL << i))
            ApicSendTlbShootdownIpi(i);
    }

    TlbShootdownWait(myCpu, targetMask, targetCr3, virtualAddr);

    IrqSpinLockRelease(&g_tlbRequest.lock, flags);
}

void TlbShootdownFull(uint64_t targetCr3, uint64_t cpuMask)
{
    if (SmpGetCpuCount() <= 1)
        return;

    // Local full flush
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");

    uint32_t myCpu = SmpCurrentCpuIndex();
    uint32_t cpuCount = SmpGetCpuCount();

    // Intersect cpuMask with online CPUs, exclude self
    uint64_t targetMask = 0;
    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (i == myCpu) continue;
        if (!(cpuMask & (1ULL << i))) continue;
        const CpuInfo* info = SmpGetCpu(i);
        if (info && info->online)
            targetMask |= (1ULL << i);
    }

    if (targetMask == 0)
        return;

    uint32_t targetCount = __builtin_popcountll(targetMask);

    uint64_t flags = IrqSpinLockAcquire(&g_tlbRequest.lock);

    g_tlbRequest.targetCr3 = targetCr3;
    g_tlbRequest.addr      = 0;  // 0 = full flush
    __atomic_store_n(&g_tlbRequest.ackBitmap, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tlbRequest.pendingCount, targetCount, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (targetMask & (1ULL << i))
            ApicSendTlbShootdownIpi(i);
    }

    TlbShootdownWait(myCpu, targetMask, targetCr3, 0);

    IrqSpinLockRelease(&g_tlbRequest.lock, flags);
}

// ---------------------------------------------------------------------------
// I/O APIC
// ---------------------------------------------------------------------------
//
// The I/O APIC is accessed through two MMIO registers:
//   IOREGSEL (offset 0x00): write the register index to read/write.
//   IOWIN    (offset 0x10): read or write the selected register.
//
// Redirection table entries start at register index 0x10 (entry 0 low) and
// are 64 bits wide, accessed as two consecutive 32-bit registers:
//   lo = 0x10 + 2 * entry
//   hi = 0x11 + 2 * entry
//
// Each entry format:
//   bits  7:0  = delivery vector
//   bits 10:8  = delivery mode (000 = fixed)
//   bit  11    = destination mode (0 = physical APIC ID)
//   bit  13    = polarity (0 = active high)
//   bit  15    = trigger mode (0 = edge, 1 = level)
//   bit  16    = mask (1 = masked)
//   bits 63:56 = destination APIC ID (in hi register)

static uint64_t  g_ioApicVirt   = 0;
static uint32_t  g_ioApicGsiBase = 0;

static inline void IoApicWrite(uint8_t reg, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t*>(g_ioApicVirt + 0x00) = reg;
    *reinterpret_cast<volatile uint32_t*>(g_ioApicVirt + 0x10) = val;
}

static inline uint32_t IoApicRead(uint8_t reg)
{
    *reinterpret_cast<volatile uint32_t*>(g_ioApicVirt + 0x00) = reg;
    return *reinterpret_cast<volatile uint32_t*>(g_ioApicVirt + 0x10);
}

bool IoApicInit(uint64_t ioApicPhysical, uint32_t gsiBase)
{
    if (ioApicPhysical == 0)
    {
        SerialPuts("IOAPIC: no I/O APIC address\n");
        return false;
    }

    g_ioApicVirt = VmmAllocPages(1, VMM_WRITABLE, MemTag::Device, KernelPid).raw();
    if (g_ioApicVirt == 0)
    {
        SerialPuts("IOAPIC: failed to allocate virtual page\n");
        return false;
    }

    PhysicalAddress oldPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(g_ioApicVirt));
    VmmUnmapPage(KernelPageTable, VirtualAddress(g_ioApicVirt));
    if (oldPhys) PmmFreePage(oldPhys);

    if (!VmmMapPage(KernelPageTable, VirtualAddress(g_ioApicVirt), PhysicalAddress(ioApicPhysical),
                    VMM_WRITABLE | VMM_NO_EXEC,
                    MemTag::Device, KernelPid))
    {
        SerialPuts("IOAPIC: failed to map MMIO\n");
        return false;
    }

    g_ioApicGsiBase = gsiBase;

    uint32_t version  = IoApicRead(0x01);
    uint32_t maxEntry = (version >> 16) & 0xFF;

    // Mask all redirection entries at startup.
    for (uint32_t i = 0; i <= maxEntry; ++i)
    {
        IoApicWrite(static_cast<uint8_t>(0x10 + 2 * i),     0x00010000u); // masked
        IoApicWrite(static_cast<uint8_t>(0x11 + 2 * i),     0x00000000u);
    }

    SerialPrintf("IOAPIC: mapped phys 0x%p → virt 0x%p, %u entries, GSI base %u\n",
                 reinterpret_cast<void*>(ioApicPhysical),
                 reinterpret_cast<void*>(g_ioApicVirt),
                 maxEntry + 1u, gsiBase);
    return true;
}

void IoApicUnmaskIrq(uint8_t irq, uint8_t vector)
{
    uint32_t entry = irq;
    uint8_t  dest  = ApicGetId();

    // hi: destination LAPIC ID in bits 31:24
    IoApicWrite(static_cast<uint8_t>(0x11 + 2 * entry),
                static_cast<uint32_t>(dest) << 24);
    // lo: vector, fixed delivery, edge-triggered, active-high, unmasked
    IoApicWrite(static_cast<uint8_t>(0x10 + 2 * entry),
                static_cast<uint32_t>(vector));
}

void IoApicMaskIrq(uint8_t irq)
{
    uint32_t entry = irq;
    uint32_t lo = IoApicRead(static_cast<uint8_t>(0x10 + 2 * entry));
    IoApicWrite(static_cast<uint8_t>(0x10 + 2 * entry), lo | 0x00010000u);
}

void ApicInitTimerOnAp()
{
    // Software-enable the LAPIC on this AP and zero the Task Priority
    // Register so it accepts all interrupt classes. Without SVR bit 8
    // set the LAPIC silently drops the timer IRQs we're about to ask
    // it to deliver, which leaves the AP stuck in its startup hlt loop
    // forever (manifests as ~30% of boots hanging at "COMPOSITOR: thread
    // started" once everything else has gone idle).
    LapicWrite(LapicReg::SVR,
               LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    LapicWrite(LapicReg::TPR, 0);

    // Start the LAPIC timer on this AP using the BSP's calibrated ticks/ms.
    // The LAPIC is already enabled and MMIO-mapped (shared virtual address).
    LapicWrite(LapicReg::TIMER_DIVIDE, 0x3);  // divide by 16
    LapicWrite(LapicReg::LVT_TIMER,
               LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    LapicWrite(LapicReg::TIMER_INIT_CNT, g_timerTicksPerMs);  // 1ms period
}

uint64_t ApicTickCount()
{
    return g_lapicTickCount;
}

} // namespace brook

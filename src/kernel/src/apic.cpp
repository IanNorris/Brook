#include "apic.h"
#include "idt.h"
#include "gs_paranoid.h"
#include "fpu_irq.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "serial.h"
#include "portio.h"
#include "smp.h"
#include "spinlock.h"
#include "panic.h"

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

// Validate the iret frame on the stack before iretq.  Called from the naked
// timer ISR after LapicTimerHandlerInner returns but before we pop registers.
// The iret frame starts 120 bytes above the current RSP (15 pushed GPRs).
//
// frame layout: [RIP, CS, RFLAGS, RSP, SS]  (each 8 bytes)
extern "C" __attribute__((used))
void ValidateIretFrame(const uint64_t* frame)
{
    uint64_t rip    = frame[0];
    uint64_t cs     = frame[1];
    uint64_t rflags = frame[2];
    uint64_t rsp    = frame[3];
    uint64_t ss     = frame[4];

    bool bad = false;

    // Validate CS is a known selector
    if (cs != 0x08 && cs != 0x2B && cs != 0x28)
    {
        SerialPrintf("!!! IRET VALIDATE: bad CS=0x%lx\n", cs);
        bad = true;
    }

    // Validate SS matches expected value for the privilege level
    uint64_t expectedSs = (cs & 3) ? 0x23 : 0x10;
    if (ss != expectedSs && ss != 0)
    {
        SerialPrintf("!!! IRET VALIDATE: bad SS=0x%lx (expected 0x%lx) CS=0x%lx\n",
                     ss, expectedSs, cs);
        bad = true;
    }

    // Validate RIP is canonical
    uint64_t ripTop = rip >> 47;
    if (ripTop != 0 && ripTop != 0x1FFFF)
    {
        SerialPrintf("!!! IRET VALIDATE: non-canonical RIP=0x%lx\n", rip);
        bad = true;
    }

    // Validate RSP is canonical
    uint64_t rspTop = rsp >> 47;
    if (rspTop != 0 && rspTop != 0x1FFFF)
    {
        SerialPrintf("!!! IRET VALIDATE: non-canonical RSP=0x%lx\n", rsp);
        bad = true;
    }

    // Check GDT entry for SS=0x10 (kernel data segment)
    // Read GDTR to get GDT base
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    auto* gdt = reinterpret_cast<const uint8_t*>(gdtr.base);
    // GDT[2] = offset 0x10, access byte is at offset 5 within the entry
    uint8_t access = gdt[0x10 + 5];
    if (access != 0x92 && access != 0x93)
    {
        SerialPrintf("!!! IRET VALIDATE: GDT[2] access=0x%x (expected 0x92/0x93)\n",
                     access);
        bad = true;
    }

    if (bad)
    {
        SerialPrintf("!!! IRET frame: RIP=0x%lx CS=0x%lx RFLAGS=0x%lx RSP=0x%lx SS=0x%lx\n",
                     rip, cs, rflags, rsp, ss);
        SerialPrintf("!!! GDT base=0x%lx limit=0x%x\n", gdtr.base, gdtr.limit);
        // Dump GDT entries 0-5
        auto* gdtEntries = reinterpret_cast<const uint64_t*>(gdtr.base);
        for (int i = 0; i < 6; i++)
            SerialPrintf("!!!   GDT[%d] = 0x%016lx\n", i, gdtEntries[i]);

        // Halt this CPU
        __asm__ volatile("cli\n\thlt");
    }
}

// C handler called from the naked ISR wrapper below.
// interruptedRip/interruptedCs/interruptedRbp are passed from the naked
// handler (extracted from the CPU interrupt frame on the stack).
void TlbEpochFlushLocal();  // BRO-179 passive epoch flush (defined below)
extern "C" void LapicTimerHandlerInner(uint64_t interruptedRip, uint64_t interruptedCs,
                                    uint64_t interruptedRbp)
{
    // BRO-166 invariant guard. In ring 0 the active GS base must be this CPU's
    // KernelCpuEnv (a kernel-canonical, high-half address). If a SWAPGS
    // imbalance left it at the user value (0, or any non-kernel address) the
    // first gs-relative access below (e.g. SmpCurrentCpuIndex's gs:176) would
    // #PF at a low address — or silently read garbage and corrupt g_perCpu[].
    // Read the base via rdmsr (NOT gs-relative — a gs read would itself fault
    // when the base is 0) and fail loudly with the interrupted context instead.
    {
        constexpr uint32_t IA32_GS_BASE_MSR = 0xC0000101;
        uint64_t gsBase = ReadMsr(IA32_GS_BASE_MSR);
        if (gsBase < 0xFFFF800000000000ULL)
        {
            KernelPanic("BRO-166: LAPIC timer ISR with bad GS base 0x%lx — "
                        "interrupted RIP=0x%lx CS=0x%lx RBP=0x%lx",
                        gsBase, interruptedRip, interruptedCs, interruptedRbp);
        }
    }

    LapicWrite(LapicReg::EOI, 0);

    // BRO-179: passive TLB-epoch flush. If the quarantine drainer has bumped the
    // epoch, this CPU does its full flush here (IF=1 safe point) and publishes,
    // letting the drainer release frames once every CPU has flushed. Cheap no-op
    // when up to date.
    TlbEpochFlushLocal();

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
        // BRO-178: GPRs first, THEN paranoid swapgs (rax/rcx/rdx saved for rdmsr,
        // ebx carries the did-swap flag). The old CS-RPL test ran before the
        // pushes and mis-decided when an IPI/timer hit a ring-0 user-GS window.
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

        GS_PARANOID_ENTRY_EBX

        // BRO-187: preserve the interrupted thread's x87/SSE/AVX state on an
        // aligned slot on its kernel stack before the handler clobbers vector
        // registers. Sets r14 = original RSP (GPR base), r15 = XSAVE slot.
        BROOK_FPU_SAVE_IRQ

        // Stack layout after 15 pushes (read via r14 = saved GPR base, since
        // RSP now points at the XSAVE slot):
        //   r14+64  = interrupted RBP
        //   r14+120 = interrupted RIP
        //   r14+128 = interrupted CS
        "movq 120(%%r14), %%rdi\n\t"    // arg1 = interrupted RIP
        "movq 128(%%r14), %%rsi\n\t"    // arg2 = interrupted CS
        "movq 64(%%r14), %%rdx\n\t"     // arg3 = interrupted RBP

        "cld\n\t"
        "call LapicTimerHandlerInner\n\t"

        // Validate iret frame before restoring GPRs.
        // The frame is at r14+120 (15 pushed regs × 8 bytes from the GPR base).
        "leaq 120(%%r14), %%rdi\n\t"
        "call ValidateIretFrame\n\t"

        // BRO-187: restore the interrupted FPU state AFTER all handler C calls
        // (which clobber vector regs), then restore RSP to the GPR base.
        BROOK_FPU_RESTORE_IRQ

        // BRO-178: paranoid swapgs restore (ebx flag) BEFORE popping GPRs.
        GS_PARANOID_EXIT_EBX

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

        "iretq\n\t"
        :
        :
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
extern "C" void ReschedIpiHandlerInner(uint64_t interruptedCs)
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

        // BRO-178: paranoid swapgs by actual GS base (ebx = did-swap flag).
        GS_PARANOID_ENTRY_EBX

        // Extract interrupted CS from the interrupt frame.
        // After 15 GPR pushes the interrupt frame CS is at RSP + 128.
        "movq 128(%%rsp), %%rdi\n\t"    // arg1 = interrupted CS

        "cld\n\t"
        "call ReschedIpiHandlerInner\n\t"

        // BRO-178: paranoid swapgs restore (ebx flag) before popping GPRs.
        GS_PARANOID_EXIT_EBX

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
        "iretq\n\t"
        :
        :
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
// Protocol (BRO-192: monotonic-generation, ABA-free, deadlock-free):
//   1. Initiator modifies PTE, does local invlpg/CR3 reload.
//   2. Initiator acquires g_tlbRequest.lock via TlbLockAcquireServicing (IF=0)
//      — while spinning for the lock it KEEPS SERVICING the in-flight request
//      and the PMM-drain epoch, so a CPU spinning here can never wedge them.
//   3. Fills {targetCr3, addr, targetMask}, then publishes a fresh monotonic
//      `generation` LAST (release). Sends TLB_SHOOTDOWN_VECTOR IPI to each target.
//   4. Waits until every target has published g_cpuTlbAckGen[i] >= generation
//      (with a timeout that forgives CPUs not running targetCr3).
//   5. Releases lock.
//
// Handler (naked ISR on target CPU) and the lock-spin self-service path BOTH
// call the single helper TlbServiceLocal(): flush as the request dictates, then
// publish g_cpuTlbAckGen[cpu] = generation.
//
// Why this is ABA-free: a CPU records the *generation* it serviced, not a bit in
// a reused bitmap. A delayed/stale service from an older generation writes a
// SMALLER value than any newer waiter requires, so it can never falsely satisfy a
// republished request — the freed-while-mapped hazard the old reused-slot
// pendingCount/ackBitmap scheme exposed (a forgive-then-republish window let a
// late acker decrement the next generation's count). Monotonicity closes it.
//
// Why this is deadlock-free: the old scheme spun IF=0 in TlbShootdownWait while
// holding the lock; a sibling thread of the SAME process (currentCr3==targetCr3)
// that was itself spinning IF=0 to acquire this lock could never service the IPI
// and was never forgiven → circular wait. Now the lock-acquire spin services the
// in-flight request itself, so it always acks while it waits.
//
// The handler must NOT acquire any lock — the initiator holds the spinlock and
// waits synchronously, so any lock attempt would deadlock.

struct TlbShootdownRequest {
    IrqSpinLock     lock;             // Must be IrqSpinLock: initiator needs IF=0 during wait
    uint64_t        targetCr3;        // only invalidate if CPU's CR3 matches
    uint64_t        addr;             // page VA to invalidate (0 = full flush)
    volatile uint64_t targetMask;     // CPUs that must service the current generation
    volatile uint32_t unconditional;  // 1 = every CPU does a full CR3 reload regardless
                                      // of targetCr3 (currently unused; epoch barrier
                                      // handles the quarantine drain instead)
    volatile uint64_t generation;     // monotonic; published LAST under lock (release)
};

static TlbShootdownRequest g_tlbRequest;

// Per-CPU: the highest shootdown generation this CPU has serviced. Monotonic —
// this is what makes the protocol ABA-free across the single reused request slot.
static volatile uint64_t g_cpuTlbAckGen[MAX_CPUS] = {};

// Service the in-flight per-AS shootdown for THIS cpu, if it is a pending target
// of the current generation. Shared by the IPI handler and the lock-acquire spin.
// Idempotent and safe at IF=0: touches only this CPU's TLB plus monotonic atomics
// and takes no lock. A torn/stale field read at a generation boundary is harmless
// (worst case: a redundant flush or a wrong invlpg address — never a missed flush
// the waiter observes, because the ack records the generation actually seen).
static inline void TlbServiceLocal(uint32_t cpu)
{
    if (cpu >= MAX_CPUS)
        return;
    uint64_t gen = __atomic_load_n(&g_tlbRequest.generation, __ATOMIC_ACQUIRE);
    if (gen == 0)
        return;
    if (__atomic_load_n(&g_cpuTlbAckGen[cpu], __ATOMIC_ACQUIRE) >= gen)
        return;
    uint64_t tmask = __atomic_load_n(&g_tlbRequest.targetMask, __ATOMIC_ACQUIRE);
    if (!(tmask & (1ULL << cpu)))
        return;

    uint64_t myCr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(myCr3));
    if (__atomic_load_n(&g_tlbRequest.unconditional, __ATOMIC_ACQUIRE))
    {
        __asm__ volatile("movq %0, %%cr3" :: "r"(myCr3) : "memory");
    }
    else if ((myCr3 & ~0xFFFULL) ==
             (__atomic_load_n(&g_tlbRequest.targetCr3, __ATOMIC_ACQUIRE) & ~0xFFFULL))
    {
        uint64_t addr = __atomic_load_n(&g_tlbRequest.addr, __ATOMIC_ACQUIRE);
        if (addr != 0)
            __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
        else
            __asm__ volatile("movq %0, %%cr3" :: "r"(myCr3) : "memory");
    }
    // else: not running the target AS — a context switch already reloaded CR3
    // (full flush), so no stale entries remain; just publish the ack below.

    // Publish AFTER the flush. RELEASE so the flush is globally ordered before any
    // waiter observes the ack. Monotonic max: never regress a newer ack.
    if (gen > __atomic_load_n(&g_cpuTlbAckGen[cpu], __ATOMIC_RELAXED))
        __atomic_store_n(&g_cpuTlbAckGen[cpu], gen, __ATOMIC_RELEASE);
}

// Timeout for the spin-wait: ~10ms at 2.5GHz ≈ 25M iterations of pause loop.
// Each pause is ~10-100 cycles, so 500K iterations ≈ 5-50ms.
static constexpr uint64_t TLB_SHOOTDOWN_TIMEOUT = 10000000; // ~10ms at 1GHz loop

// ---------------------------------------------------------------------------
// BRO-179 passive TLB-epoch barrier (replaces the old IPI-and-wait barrier).
//
// Why the old barrier deadlocked: it targeted ALL online CPUs unconditionally
// while holding g_tlbRequest.lock and spun for their ACKs. A target CPU that was
// itself trying to start a NORMAL shootdown spins on that SAME lock with IF=0
// (IrqSpinLockAcquire does cli-then-spin) and so can never service the barrier
// IPI → never ACKs → timeout. If it also held g_vmmLock/g_pmmLock it was a true
// circular deadlock (drainer waits for its ack; it waits for the lock the
// drainer holds). Normal per-AS shootdowns avoid this because they only target
// CPUs whose CR3 == targetCr3 (running that AS in user mode, IF=1), never CPUs
// spinning in-kernel on the lock.
//
// Passive epoch design (deadlock-free by construction):
//   * g_tlbEpoch is bumped when the quarantine drainer wants to release a batch.
//   * Every CPU, at a safe point (LAPIC timer tick, IF=1), runs
//     TlbEpochFlushLocal(): if it is behind the epoch it does a FULL CR3 reload
//     (CR4.PGE is OFF — verified — so this evicts ALL entries incl. kernel
//     mappings, which is the BRO-179 root) and publishes the epoch it flushed.
//   * The drainer holds NO lock while waiting; it spins (IF=1) until every CPU
//     in the snapshot's online set has published >= the target epoch, then
//     releases the batch. Generous timeout → PANIC (fail loud, never release an
//     unflushed frame — a privesc-class invariant).
// The drainer holds no lock, so a CPU in a bounded IF=0 section simply flushes on
// its next tick; normal shootdowns are completely independent of this path.
//
// NOTE: correctness here REQUIRES CR4.PGE to stay OFF (so CR3 reload is a full
// flush). Asserted at init (ApicVerifyNoGlobalPages). If PGE is ever enabled,
// this barrier must switch to a CR4.PGE toggle or INVPCID-all.
//
// LIMITATION (real HW): a CPU in deep idle without ARAT can stop its LAPIC timer
// and never advance its epoch → the drainer would time out. A fire-and-forget
// nudge IPI on epoch bump (TODO) wakes such CPUs; on KVM the periodic LAPIC
// timer fires through hlt, so the timer backstop suffices for validation.
static volatile uint64_t g_tlbEpoch = 0;
static volatile uint64_t g_cpuFlushedEpoch[MAX_CPUS] = {};

// BRO-179: set by TlbFlushAllCpusBarrier while it is NMI-escalating a stuck CPU.
// Referenced (RIP-relative) by the naked NMI handler in smp.cpp: when a non-panic
// NMI lands and this is set, the handler services this CPU's epoch flush (via
// TlbNmiFlushRecord) instead of just returning. extern "C" so the asm can name it.
extern "C" volatile uint8_t g_tlbNmiActive = 0;

// Called at a safe point (IF=1) on each CPU — currently the LAPIC timer tick.
// Cheap no-op when this CPU is already up to date with g_tlbEpoch.
void TlbEpochFlushLocal()
{
    uint32_t cpu;
    __asm__ volatile("movl %%gs:176, %0" : "=r"(cpu));
    if (cpu >= MAX_CPUS) return;
    uint64_t target = __atomic_load_n(&g_tlbEpoch, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_cpuFlushedEpoch[cpu], __ATOMIC_RELAXED) >= target)
        return;
    // Full TLB flush via CR3 reload (serializing; PGE off → evicts everything).
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");
    // Publish AFTER the flush. The CR3 reload is architecturally serializing, so
    // the flush is globally ordered before this release store.
    __atomic_store_n(&g_cpuFlushedEpoch[cpu], target, __ATOMIC_RELEASE);
}

// BRO-179: NMI-context epoch flush. Called ONLY from the naked NMI handler asm
// (smp.cpp) with the CPU index derived from the IST stack RSP — NOT from %gs,
// because an NMI can land while a user-mode GS base is loaded. This is what makes
// the epoch barrier ACTIVE: a CPU stuck in a long IF=0 region (e.g. spinning in
// SerialLockAcquire, which holds cli across its whole critical section and so
// never takes the timer tick that would call TlbEpochFlushLocal) is forced by the
// NMI to flush and publish, so it can no longer starve the PMM-drain barrier.
//
// NMI-safety (must hold — a fault here would prematurely unmask NMI and corrupt
// the shared IST stack): the whole translation unit is built -mgeneral-regs-only
// -mno-sse and the kernel is -fno-stack-protector -mno-red-zone, so this touches
// no XMM/TLS/canary; it only does a mov cr3 and an aligned atomic store to an
// always-mapped static array, neither of which can fault. Idempotent: re-entry or
// a racing timer-tick flush just re-stores the same monotone `target`.
extern "C" void TlbNmiFlushRecord(uint64_t cpuIdx)
{
    if (cpuIdx >= MAX_CPUS)
        return;
    uint64_t target = __atomic_load_n(&g_tlbEpoch, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_cpuFlushedEpoch[cpuIdx], __ATOMIC_RELAXED) >= target)
        return;
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");
    __atomic_store_n(&g_cpuFlushedEpoch[cpuIdx], target, __ATOMIC_RELEASE);
}

extern "C" void TlbShootdownHandlerInner()
{
    // Single source of truth for "satisfy the in-flight shootdown for this CPU",
    // shared with the lock-acquire self-service path (TlbLockAcquireServicing).
    uint32_t cpu;
    __asm__ volatile("movl %%gs:176, %0" : "=r"(cpu));
    TlbServiceLocal(cpu);

    LapicWrite(LapicReg::EOI, 0);
}

// Naked ISR — same structure as ReschedIpiHandler.
__attribute__((naked))
static void TlbShootdownHandler(void)
{
    __asm__ volatile(
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

        // BRO-178: paranoid swapgs by actual GS base (ebx = did-swap flag).
        // This is the handler that was crashing at gs:176 (CR2=0xB0) when a
        // TLB-shootdown IPI arrived during a ring-0 user-GS window.
        GS_PARANOID_ENTRY_EBX

        "cld\n\t"
        "call TlbShootdownHandlerInner\n\t"

        // BRO-178: paranoid swapgs restore (ebx flag) before popping GPRs.
        GS_PARANOID_EXIT_EBX

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
        "iretq\n\t"
        :
        :
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
                                      uint64_t gen, uint64_t targetCr3,
                                      uint64_t addr)
{
    uint32_t cpuCount = SmpGetCpuCount();
    uint64_t notAcked = 0;
    for (uint32_t i = 0; i < cpuCount; i++)
        if ((targetMask & (1ULL << i)) &&
            __atomic_load_n(&g_cpuTlbAckGen[i], __ATOMIC_ACQUIRE) < gen)
            notAcked |= (1ULL << i);

    SerialPrintf("\n!!! TLB_SHOOTDOWN: TIMEOUT — deadlock detected !!!\n");
    SerialPrintf("  initiator: CPU %u\n", myCpu);
    SerialPrintf("  targetCr3: 0x%lx  addr: 0x%lx  generation: %lu\n", targetCr3, addr, gen);
    SerialPrintf("  targetMask: 0x%lx  not-acked: 0x%lx\n", targetMask, notAcked);
    SerialPrintf("  not-acked CPUs (IF=0, holding lock?):\n");

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

    // Halt all APs and panic
    SerialPrintf("KERNEL PANIC: TLB shootdown deadlock\n");
    SmpHaltAllAPs();
    while (true) __asm__ volatile("hlt");
}

// Self-servicing acquire for g_tlbRequest.lock. It is a ticket lock (see
// spinlock.h); while we spin IF=0 waiting for our turn we KEEP servicing the
// in-flight shootdown and the PMM-drain epoch for THIS cpu. That is what breaks
// the BRO-192 same-CR3 deadlock: a sibling thread spinning here still acks the
// current holder's request (and advances the drain epoch) instead of wedging it.
static uint64_t TlbLockAcquireServicing(uint32_t myCpu)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    uint32_t ticket = __atomic_fetch_add(&g_tlbRequest.lock.next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&g_tlbRequest.lock.serving, __ATOMIC_ACQUIRE) != ticket)
    {
        TlbServiceLocal(myCpu);     // ack the current holder's request if targeted
        TlbEpochFlushLocal();       // keep the PMM-drain epoch advancing too
        __asm__ volatile("pause" ::: "memory");
    }

    if (myCpu < 64)
    {
        g_lockDiag[myCpu].file = __FILE__;
        g_lockDiag[myCpu].line = __LINE__;
        __atomic_store_n(&g_lockDiag[myCpu].held, 1u, __ATOMIC_RELEASE);
    }
    return flags;
}

// Spin until every target CPU has published g_cpuTlbAckGen[i] >= gen. After a
// timeout, forgive CPUs not running targetCr3 (a context switch reloads CR3 = a
// full flush, so they hold no stale entries); CPUs still on targetCr3 MUST ack to
// guarantee no stale writable entry survives (critical for CoW fork). Self-service
// in the lock-acquire spin guarantees such CPUs eventually ack, so this no longer
// livelocks. We also advance our own drain epoch while waiting IF=0.
static void TlbShootdownWait(uint32_t myCpu, uint64_t targetMask,
                              uint64_t targetCr3, uint64_t addr, uint64_t gen)
{
    uint32_t cpuCount = SmpGetCpuCount();
    uint64_t forgivenMask = 0;
    uint64_t spins = 0;
    for (;;)
    {
        uint64_t remaining = 0;
        for (uint32_t i = 0; i < cpuCount; i++)
        {
            if (i == myCpu || !(targetMask & (1ULL << i))) continue;
            if (forgivenMask & (1ULL << i)) continue;
            if (__atomic_load_n(&g_cpuTlbAckGen[i], __ATOMIC_ACQUIRE) < gen)
                remaining |= (1ULL << i);
        }
        if (remaining == 0)
            return;

        TlbEpochFlushLocal();
        __asm__ volatile("pause" ::: "memory");
        if (++spins > TLB_SHOOTDOWN_TIMEOUT)
        {
            uint32_t forgiven = 0;
            uint32_t stillWaiting = 0;
            for (uint32_t i = 0; i < cpuCount; i++)
            {
                if (!(remaining & (1ULL << i))) continue;

                const CpuInfo* info = SmpGetCpu(i);
                uint64_t cpuCr3 = info ? __atomic_load_n(&info->currentCr3, __ATOMIC_ACQUIRE) : 0;
                bool onTargetCr3 = cpuCr3 && ((cpuCr3 & ~0xFFFULL) == (targetCr3 & ~0xFFFULL));

                if (!onTargetCr3)
                {
                    forgivenMask |= (1ULL << i);
                    forgiven++;
                }
                else
                {
                    // Re-IPI in case the original shootdown IPI was lost; the CPU's
                    // own lock-acquire self-service will also ack it.
                    ApicSendTlbShootdownIpi(i);
                    stillWaiting++;
                }
            }

            if (stillWaiting == 0)
            {
                if (forgiven > 0 && !g_hotLogQuiet)
                    SerialPrintf("TLB_SHOOTDOWN: forgave %u CPU(s) (different CR3), cr3=0x%lx addr=0x%lx\n",
                                 forgiven, targetCr3, addr);
                return;
            }
            if (!g_hotLogQuiet)
                SerialPrintf("TLB_SHOOTDOWN: forgave %u, waiting for %u on target CR3=0x%lx\n",
                             forgiven, stillWaiting, targetCr3);
            spins = 0;
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

    uint64_t flags = TlbLockAcquireServicing(myCpu);

    g_tlbRequest.targetCr3 = targetCr3;
    g_tlbRequest.addr      = virtualAddr;
    __atomic_store_n(&g_tlbRequest.unconditional, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tlbRequest.targetMask, targetMask, __ATOMIC_RELAXED);
    // Publish a fresh generation LAST (ACQ_REL = full fence on x86): orders all the
    // field stores above before any target can observe the new generation.
    uint64_t gen = __atomic_add_fetch(&g_tlbRequest.generation, 1, __ATOMIC_ACQ_REL);

    // Send IPI to each target
    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (targetMask & (1ULL << i))
            ApicSendTlbShootdownIpi(i);
    }

    TlbShootdownWait(myCpu, targetMask, targetCr3, virtualAddr, gen);

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

    uint64_t flags = TlbLockAcquireServicing(myCpu);

    g_tlbRequest.targetCr3 = targetCr3;
    g_tlbRequest.addr      = 0;  // 0 = full flush
    __atomic_store_n(&g_tlbRequest.unconditional, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tlbRequest.targetMask, targetMask, __ATOMIC_RELAXED);
    uint64_t gen = __atomic_add_fetch(&g_tlbRequest.generation, 1, __ATOMIC_ACQ_REL);

    for (uint32_t i = 0; i < cpuCount; i++)
    {
        if (targetMask & (1ULL << i))
            ApicSendTlbShootdownIpi(i);
    }

    TlbShootdownWait(myCpu, targetMask, targetCr3, 0, gen);

    IrqSpinLockRelease(&g_tlbRequest.lock, flags);
}

// BRO-179 quarantine drain barrier — passive epoch implementation (see the
// design note above TlbEpochFlushLocal). Bumps the global epoch, flushes self,
// then waits (holding NO lock) until every online CPU has flushed since the bump.
// PANICS on timeout — never returns having left a CPU un-flushed, because the
// drainer releases physical frames back to the allocator only after this returns.
//
// MUST be called from a context with IF=1 and NO IrqSpinLock held (it busy-waits
// for remote CPUs to reach their next safe point). The PMM drain thread satisfies
// this.
void TlbFlushAllCpusBarrier()
{
    // One-time precondition check: the CR3-reload flush is a FULL flush only when
    // global pages are disabled (CR4.PGE off). The panel flagged this as the
    // single thing that would silently defeat the fix. Verify once; fail loud if
    // PGE is ever enabled so this is caught immediately, not as recurring SIG1.
    static volatile bool s_pgeChecked = false;
    if (!s_pgeChecked)
    {
        uint64_t cr4;
        __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
        if (cr4 & (1ULL << 7))
            KernelPanic("BRO-179: CR4.PGE is set — epoch barrier's CR3 reload no "
                        "longer flushes global kernel pages; switch to CR4.PGE "
                        "toggle or INVPCID-all (cr4=0x%lx)", cr4);
        s_pgeChecked = true;
    }

    // Always flush self first (covers uniprocessor and the drainer's own CPU).
    {
        uint64_t cr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");
    }

    uint32_t cpuCount = SmpGetCpuCount();
    if (cpuCount <= 1)
        return;

    uint32_t myCpu = SmpCurrentCpuIndex();

    // Snapshot the online set we will wait on.
    uint64_t onlineMask = 0;
    for (uint32_t i = 0; i < cpuCount; i++)
    {
        const CpuInfo* info = SmpGetCpu(i);
        if (info && info->online)
            onlineMask |= (1ULL << i);
    }

    // Bump the epoch: every CPU must perform a full flush AFTER this point before
    // any frame freed before this point may be reused.
    uint64_t target = __atomic_add_fetch(&g_tlbEpoch, 1, __ATOMIC_ACQ_REL);

    // Self is flushed as of the reload above (which happened after the bump's
    // read-modify-write is irrelevant — self already has a fresh TLB).
    if (myCpu < MAX_CPUS)
        __atomic_store_n(&g_cpuFlushedEpoch[myCpu], target, __ATOMIC_RELEASE);

    // Wait, holding NO lock, until every other online CPU has flushed >= target.
    // A CPU normally reaches this passively: its LAPIC timer tick (IF=1) runs
    // TlbEpochFlushLocal() within a bounded time of leaving any IF=0 section. But
    // a CPU stuck in a LONG IF=0 region that never calls TlbEpochFlushLocal (e.g.
    // spinning in SerialLockAcquire, which holds cli across its whole critical
    // section) would never flush and would starve this barrier (BRO-179 timeout
    // panic). So after a passive grace period we ESCALATE: send a targeted NMI to
    // each still-missing CPU. The NMI is delivered even with IF=0 and, while
    // g_tlbNmiActive is set, the handler runs TlbNmiFlushRecord() to flush+publish
    // for that CPU. Deadlock-free: we hold no lock, so no CPU can be blocked on us.
    static constexpr uint32_t BARRIER_MAX_NMI_ROUNDS = 12; // ~0.5s total budget
    uint64_t spins = 0;
    uint32_t nmiRounds = 0;
    bool nmiArmed = false;
    for (;;)
    {
        uint64_t missing = 0;
        for (uint32_t i = 0; i < cpuCount; i++)
        {
            if (i == myCpu || !(onlineMask & (1ULL << i))) continue;
            if (__atomic_load_n(&g_cpuFlushedEpoch[i], __ATOMIC_ACQUIRE) < target)
                missing |= (1ULL << i);
        }
        if (missing == 0)
        {
            if (nmiArmed)
                __atomic_store_n(&g_tlbNmiActive, 0u, __ATOMIC_RELEASE);
            return;
        }

        __asm__ volatile("pause" ::: "memory");

        // First escalation after a short passive grace (~10ms); each subsequent
        // NMI round waits longer (~40ms) and re-sends, to ride out a CPU briefly
        // trapped in an SMI (which masks NMI) before declaring a genuine wedge.
        const uint64_t limit = (nmiRounds == 0)
                                   ? TLB_SHOOTDOWN_TIMEOUT
                                   : (TLB_SHOOTDOWN_TIMEOUT * 4);
        if (++spins > limit)
        {
            if (nmiRounds < BARRIER_MAX_NMI_ROUNDS)
            {
                // Arm the NMI epoch-service path, then poke each stuck CPU. Re-read
                // info->online so a genuinely parked AP is never targeted (it would
                // never ack and falsely trip the wedge panic below).
                __atomic_store_n(&g_tlbNmiActive, 1u, __ATOMIC_RELEASE);
                nmiArmed = true;
                for (uint32_t i = 0; i < cpuCount; i++)
                {
                    if (!(missing & (1ULL << i))) continue;
                    const CpuInfo* info = SmpGetCpu(i);
                    if (info && info->online)
                        ApicSendNmi(info->apicId);
                }
                nmiRounds++;
                spins = 0;
                continue;
            }
            // NMI escalation exhausted: a CPU is genuinely wedged (or in an
            // unbounded SMI). Refuse to release frames; fail loud.
            __atomic_store_n(&g_tlbNmiActive, 0u, __ATOMIC_RELEASE);
            KernelPanic("BRO-179: TLB epoch barrier timeout — online=0x%lx missing=0x%lx "
                        "target=%lu (a CPU never flushed even after %u NMI rounds; "
                        "refusing to release frames unsafely)",
                        onlineMask, missing, target, nmiRounds);
        }
    }
}
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

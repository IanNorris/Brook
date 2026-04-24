#include "apic.h"
#include "idt.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "serial.h"
#include "portio.h"

namespace brook {

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
void SchedulerTimerTick();

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

        // Re-check CMOS every ~1024 ticks (~1 second) to correct for
        // LAPIC calibration drift under host turbo / KVM dilation.
        if ((g_lapicTickCount & 0x3FF) == 0)
        {
            RtcRecalibrateLapic();
        }
    }

    // Record a profiler sample (fast no-op when profiling is disabled).
    ProfilerSample(interruptedRip, interruptedCs, interruptedRbp);

    // Drive the scheduler on every CPU.
    SchedulerTimerTick();
}

// Naked ISR entry for LAPIC timer (vector 32).
// Performs swapgs when entering from ring 3 so that gs-relative kernel data
// (KernelCpuEnv at gs:0) is accessible throughout the handler.
// Passes the interrupted RIP (rdi) and CS (rsi) to the inner handler for
// profiler sampling.
//
// IMPORTANT: The inner handler (and anything it calls, e.g. the scheduler)
// may clobber XMM registers.  We must save and restore the full FPU/SSE
// state so that user-space code (especially glibc, which uses SSE heavily
// for memset/memcpy/strcmp) is not corrupted.
__attribute__((naked))
static void LapicTimerHandler(void)
{
    __asm__ volatile(
        // If interrupted from ring 3, CS (at RSP+8) has CPL bits set.
        "testq $3, 8(%%rsp)\n\t"
        "jz 1f\n\t"
        "swapgs\n\t"
        "1:\n\t"

        // Save all caller-saved GPRs (callee-saved are preserved by C ABI).
        "push %%rax\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"

        // Save FPU/SSE state (512 bytes, must be 16-byte aligned).
        // After 9 pushes (72 bytes) + CPU frame (40 bytes for ring-3 or
        // 24 bytes for ring-0), RSP may not be 16-byte aligned.
        // Align down, save original RSP, then fxsave.
        "movq %%rsp, %%rax\n\t"         // save original RSP
        "subq $512, %%rsp\n\t"          // reserve 512 bytes for fxsave
        "andq $-16, %%rsp\n\t"          // 16-byte align
        "fxsave (%%rsp)\n\t"
        "push %%rax\n\t"                // save original RSP below fxsave area

        // Stack layout after 9 pushes (72 bytes) + fxsave:
        //   RSP+0       = saved original RSP (before fxsave alloc)
        //   RSP+8       = fxsave area (512 bytes, 16-byte aligned)
        //   ... (GPRs)
        //   (original RSP)+72  = interrupted RIP
        //   (original RSP)+80  = interrupted CS
        // Load from saved original RSP.
        "movq (%%rsp), %%rax\n\t"       // rax = original RSP after GPR pushes
        "movq 72(%%rax), %%rdi\n\t"     // arg1 = interrupted RIP
        "movq 80(%%rax), %%rsi\n\t"     // arg2 = interrupted CS
        "movq %%rbp, %%rdx\n\t"         // arg3 = interrupted RBP

        "cld\n\t"
        "call %P0\n\t"

        // Restore FPU/SSE state
        "pop %%rax\n\t"                 // rax = original RSP after GPR pushes
        "fxrstor (%%rsp)\n\t"
        "movq %%rax, %%rsp\n\t"         // restore RSP to after GPR pushes

        // Restore GPRs
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
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
    // Wait for previous IPI to be delivered
    while (LapicRead(LapicReg::ICR_LO) & (1u << 12))
        __asm__ volatile("pause");

    // Shorthand = 11 (all excluding self), delivery mode = NMI
    LapicWrite(LapicReg::ICR_LO, (0x3 << 18) | (0x4 << 8) | (1u << 14));
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

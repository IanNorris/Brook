// profiler.cpp — Sampling profiler implementation
//
// See profiler.h for overview.  The hot path (ProfilerSample) runs in ISR
// context on every CPU and must be lock-free and SSE-free.
//
// Design: per-CPU ring buffers accumulate samples during profiling.
// When profiling stops, the drain thread dumps ALL samples to the serial
// port in a parseable text format:
//   PROF_BEGIN <cpuCount> <startTick>
//   P <tick> <pid_hex> <cpu> <flags> <rip_hex>
//   ...
//   PROF_END <totalSamples> <dropped>
// A host-side script (profiler_to_speedscope.py) extracts these lines from
// the serial log and converts to Speedscope JSON.

#include "profiler.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
#include "serial.h"

// LAPIC tick counter (defined in apic.cpp).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

namespace brook {

// ---------------------------------------------------------------------------
// Sample record — variable-length stack trace
// ---------------------------------------------------------------------------

static constexpr uint32_t MAX_STACK_DEPTH = 8;

struct ProfileSample {
    uint32_t tick;                     // relative to profiler start
    uint16_t pid;
    uint8_t  cpu;
    uint8_t  flags;                    // bit 0: ring (0=kernel, 1=user)
    uint64_t rip[MAX_STACK_DEPTH];     // [0] = leaf, [1..] = callers via RBP chain
    uint8_t  depth;                    // number of valid frames
};

// ---------------------------------------------------------------------------
// Per-CPU lock-free ring buffer (single-producer ISR, single-consumer thread)
// ---------------------------------------------------------------------------
// Each sample is ~80 bytes with 8 stack frames.
// 512 samples per CPU, drained every 100ms by the profiler thread.

static constexpr uint32_t SAMPLES_PER_CPU = 512;

struct PerCpuBuffer {
    ProfileSample samples[SAMPLES_PER_CPU];
    volatile uint32_t writeIdx;   // ISR increments (mod SAMPLES_PER_CPU)
    uint32_t          readIdx;    // consumer thread advances
    uint32_t          dropped;    // samples dropped when buffer full
};

static constexpr uint32_t MAX_PROFILER_CPUS = 8;
static PerCpuBuffer g_cpuBuf[MAX_PROFILER_CPUS];

// ---------------------------------------------------------------------------
// Global profiler state
// ---------------------------------------------------------------------------

static volatile bool     g_profilerEnabled  = false;
static volatile uint64_t g_profilerStartTick = 0;
static volatile uint64_t g_profilerEndTick   = 0;  // 0 = no auto-stop
static volatile bool     g_profilerFlushReq  = false;
static Process*          g_profilerThread    = nullptr;

// ---------------------------------------------------------------------------
// ISR hot path — called from LAPIC timer handler on every CPU
// ---------------------------------------------------------------------------

// Sample every Nth tick to reduce data volume.  10 = one sample per 10ms
// per CPU.  8 CPUs × 100 Hz × 8s = 6400 samples ≈ 100 KB on disk.
static constexpr uint32_t SAMPLE_DIVIDER = 10;

void ProfilerSample(uint64_t interruptedRip, uint64_t interruptedCs, uint64_t interruptedRbp)
{
    if (!g_profilerEnabled) return;

    // Auto-stop after duration
    uint64_t now = g_lapicTickCount;
    if (g_profilerEndTick != 0 && now >= g_profilerEndTick) {
        g_profilerEnabled = false;
        g_profilerFlushReq = true;
        return;
    }

    // Only sample every Nth tick to keep data small enough for disk write
    if ((now % SAMPLE_DIVIDER) != 0) return;

    uint32_t cpu = SmpCurrentCpuIndex();
    if (cpu >= MAX_PROFILER_CPUS) return;

    PerCpuBuffer& buf = g_cpuBuf[cpu];

    // Check if buffer is full (leave 1 slot gap for SPSC safety)
    uint32_t wi = buf.writeIdx;
    uint32_t nextWi = (wi + 1) % SAMPLES_PER_CPU;
    if (nextWi == buf.readIdx) {
        buf.dropped++;
        return;
    }

    Process* proc = ProcessCurrent();
    uint16_t pid = proc ? proc->pid : 0xFFFF;
    bool userMode = (interruptedCs & 3) != 0;

    ProfileSample& s = buf.samples[wi];
    s.tick  = static_cast<uint32_t>(now - g_profilerStartTick);
    s.pid   = pid;
    s.cpu   = static_cast<uint8_t>(cpu);
    s.flags = userMode ? 1 : 0;

    // Frame 0 = leaf (interrupted RIP)
    s.rip[0] = interruptedRip;
    uint8_t depth = 1;

    // Walk frame pointer chain for kernel-mode samples only.
    // User-mode RBP might be invalid or in a different address space.
    if (!userMode && interruptedRbp != 0) {
        uint64_t rbp = interruptedRbp;
        // Kernel addresses are >= 0xffffffff80000000
        constexpr uint64_t KERNEL_BASE = 0xffffffff80000000ULL;
        constexpr uint64_t KERNEL_END  = 0xffffffffffffffffULL;
        while (depth < MAX_STACK_DEPTH) {
            // Validate RBP is in kernel range and aligned
            if (rbp < KERNEL_BASE || rbp >= KERNEL_END - 16 || (rbp & 7) != 0)
                break;
            // RBP points to: [saved_rbp, return_addr]
            const uint64_t* frame = reinterpret_cast<const uint64_t*>(rbp);
            uint64_t retAddr = frame[1];
            if (retAddr < KERNEL_BASE || retAddr >= KERNEL_END)
                break;
            s.rip[depth++] = retAddr;
            uint64_t nextRbp = frame[0];
            if (nextRbp <= rbp) break; // stack grows down; prevent loops
            rbp = nextRbp;
        }
    }
    s.depth = depth;

    // Release store ensures sample data is visible before advancing writeIdx
    __atomic_store_n(&buf.writeIdx, nextWi, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Hex digit table for serial output
// ---------------------------------------------------------------------------

static const char kHexDigits[] = "0123456789abcdef";

// ---------------------------------------------------------------------------
// Drain samples to serial in parseable text format
// ---------------------------------------------------------------------------
// Output format (one line per sample, stack frames separated by semicolons):
//   P <tick_dec> <pid_hex> <cpu> <flags> <rip0_hex>;<rip1_hex>;...
// Delimited by PROF_BEGIN / PROF_END markers.

static void SerialPutHex16(uint64_t v)
{
    for (int i = 15; i >= 0; --i)
        SerialPutChar(kHexDigits[(v >> (i * 4)) & 0xF]);
}

static void SerialPutDec(uint32_t v)
{
    char tb[11]; int ti = 10; tb[ti] = '\0';
    if (v == 0) { tb[--ti] = '0'; }
    else { while (v > 0) { tb[--ti] = '0' + (v % 10); v /= 10; } }
    const char* p = &tb[ti];
    while (*p) SerialPutChar(*p++);
}

static uint32_t DrainToSerial()
{
    uint32_t totalSamples = 0;
    uint32_t cpuCount = SmpGetCpuCount();

    for (uint32_t c = 0; c < cpuCount; c++) {
        PerCpuBuffer& buf = g_cpuBuf[c];

        uint32_t ri = buf.readIdx;
        uint32_t wi = __atomic_load_n(&buf.writeIdx, __ATOMIC_ACQUIRE);
        if (ri == wi) continue;

        SerialLock();
        while (ri != wi) {
            const ProfileSample& s = buf.samples[ri];

            SerialPutChar('P');
            SerialPutChar(' ');
            SerialPutDec(s.tick);
            SerialPutChar(' ');
            SerialPutChar(kHexDigits[(s.pid >> 12) & 0xF]);
            SerialPutChar(kHexDigits[(s.pid >> 8) & 0xF]);
            SerialPutChar(kHexDigits[(s.pid >> 4) & 0xF]);
            SerialPutChar(kHexDigits[s.pid & 0xF]);
            SerialPutChar(' ');
            SerialPutChar('0' + s.cpu);
            SerialPutChar(' ');
            SerialPutChar('0' + s.flags);
            SerialPutChar(' ');
            // Stack frames: rip0;rip1;rip2;...
            for (uint8_t d = 0; d < s.depth; d++) {
                if (d > 0) SerialPutChar(';');
                SerialPutHex16(s.rip[d]);
            }
            SerialPutChar('\n');

            ri = (ri + 1) % SAMPLES_PER_CPU;
            totalSamples++;
        }
        SerialUnlock();
        buf.readIdx = ri;
    }
    return totalSamples;
}

// ---------------------------------------------------------------------------
// Profiler kernel thread — waits for profiling to complete, dumps to serial
// ---------------------------------------------------------------------------

static void ProfilerThreadFn(void* /*arg*/)
{
    SerialPrintf("PROFILER: thread started\n");

    for (;;) {
        // Sleep until profiling starts
        while (!g_profilerEnabled && !g_profilerFlushReq) {
            Process* self = ProcessCurrent();
            if (self) {
                self->wakeupTick = g_lapicTickCount + 100;
                SchedulerBlock(self);
            }
        }

        if (g_profilerEnabled) {
            // Profiling is active — emit header and start continuous drain
            uint32_t cpuCount = SmpGetCpuCount();
            SerialPrintf("PROF_BEGIN %u %lu\n", cpuCount, g_profilerStartTick);

            uint32_t totalWritten = 0;

            // Continuously drain while profiling is active
            while (g_profilerEnabled) {
                totalWritten += DrainToSerial();

                Process* self = ProcessCurrent();
                if (self) {
                    // Drain every 100ms — fast enough to keep up with 100Hz per CPU
                    self->wakeupTick = g_lapicTickCount + 100;
                    SchedulerBlock(self);
                }
            }

            // Final drain after profiling stopped
            totalWritten += DrainToSerial();

            uint32_t totalDropped = 0;
            for (uint32_t c = 0; c < cpuCount; c++)
                totalDropped += g_cpuBuf[c].dropped;

            SerialPrintf("PROF_END %u %u\n", totalWritten, totalDropped);
            SerialPrintf("PROFILER: done — %u samples, %u dropped\n", totalWritten, totalDropped);

            g_profilerFlushReq = false;

            // Reset buffers for next run
            for (uint32_t c = 0; c < cpuCount; c++) {
                g_cpuBuf[c].writeIdx = 0;
                g_cpuBuf[c].readIdx  = 0;
                g_cpuBuf[c].dropped  = 0;
            }
        }
        else if (g_profilerFlushReq) {
            // Legacy: flush without continuous drain (shouldn't normally hit)
            g_profilerFlushReq = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ProfilerInit()
{
    // Zero all buffers
    for (uint32_t c = 0; c < MAX_PROFILER_CPUS; c++) {
        g_cpuBuf[c].writeIdx = 0;
        g_cpuBuf[c].readIdx  = 0;
        g_cpuBuf[c].dropped  = 0;
    }

    g_profilerThread = KernelThreadCreate("profiler", ProfilerThreadFn, nullptr, 3 /* low prio */);
    if (g_profilerThread) {
        SchedulerAddProcess(g_profilerThread);
        SerialPrintf("PROFILER: thread created pid=%u\n", g_profilerThread->pid);
    }
}

void ProfilerStart(uint32_t durationMs)
{
    if (g_profilerEnabled) {
        SerialPrintf("PROFILER: already running\n");
        return;
    }

    g_profilerStartTick = g_lapicTickCount;
    g_profilerEndTick   = durationMs > 0 ? (g_profilerStartTick + durationMs) : 0;
    g_profilerFlushReq  = false;

    // Enable sampling (ISRs will start recording on next tick)
    __atomic_store_n(&g_profilerEnabled, true, __ATOMIC_RELEASE);

    SerialPrintf("PROFILER: recording started (%u ms, %u CPUs)\n",
                 durationMs, SmpGetCpuCount());
}

void ProfilerStop()
{
    if (!g_profilerEnabled) return;
    g_profilerEnabled  = false;
    g_profilerFlushReq = true;
    SerialPrintf("PROFILER: stop requested\n");
}

} // namespace brook

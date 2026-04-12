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
// Sample record — 16 bytes, packed for binary output
// ---------------------------------------------------------------------------

struct __attribute__((packed)) ProfileSample {
    uint32_t tick;    // relative to profiler start
    uint16_t pid;
    uint8_t  cpu;
    uint8_t  flags;   // bit 0: ring (0=kernel, 1=user)
    uint64_t rip;
};

static_assert(sizeof(ProfileSample) == 16, "ProfileSample must be 16 bytes");

// ---------------------------------------------------------------------------
// Per-CPU lock-free ring buffer (single-producer ISR, single-consumer thread)
// ---------------------------------------------------------------------------
// 2048 samples per CPU × 16 bytes = 32 KB per CPU.
// At 100 Hz (SAMPLE_DIVIDER=10), holds ~20 seconds of data per CPU.

static constexpr uint32_t SAMPLES_PER_CPU = 2048;

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

void ProfilerSample(uint64_t interruptedRip, uint64_t interruptedCs)
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

    buf.samples[wi].tick  = static_cast<uint32_t>(now - g_profilerStartTick);
    buf.samples[wi].pid   = pid;
    buf.samples[wi].cpu   = static_cast<uint8_t>(cpu);
    buf.samples[wi].flags = userMode ? 1 : 0;
    buf.samples[wi].rip   = interruptedRip;

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
// Output format (one line per sample):
//   PROF <tick_hex> <pid_hex> <cpu> <flags> <rip_hex>
// Delimited by PROF_BEGIN / PROF_END markers.

static uint32_t DrainToSerial()
{
    uint32_t totalSamples = 0;
    uint32_t cpuCount = SmpGetCpuCount();

    for (uint32_t c = 0; c < cpuCount; c++) {
        PerCpuBuffer& buf = g_cpuBuf[c];

        uint32_t ri = buf.readIdx;
        uint32_t wi = __atomic_load_n(&buf.writeIdx, __ATOMIC_ACQUIRE);
        if (ri == wi) continue;

        // Hold serial lock for entire CPU buffer to avoid priority inversion.
        // Use SerialPutChar directly — NOT SerialPuts which re-acquires the lock.
        SerialLock();
        while (ri != wi) {
            const ProfileSample& s = buf.samples[ri];

            SerialPutChar('P');
            SerialPutChar(' ');
            // tick (decimal)
            {
                char tb[11]; int ti = 10; tb[ti] = '\0';
                uint32_t v = s.tick;
                if (v == 0) { tb[--ti] = '0'; }
                else { while (v > 0) { tb[--ti] = '0' + (v % 10); v /= 10; } }
                const char* p = &tb[ti];
                while (*p) SerialPutChar(*p++);
            }
            SerialPutChar(' ');
            // pid (4 hex digits)
            SerialPutChar(kHexDigits[(s.pid >> 12) & 0xF]);
            SerialPutChar(kHexDigits[(s.pid >> 8) & 0xF]);
            SerialPutChar(kHexDigits[(s.pid >> 4) & 0xF]);
            SerialPutChar(kHexDigits[s.pid & 0xF]);
            SerialPutChar(' ');
            // cpu (single digit)
            SerialPutChar('0' + s.cpu);
            SerialPutChar(' ');
            // flags (single digit)
            SerialPutChar('0' + s.flags);
            SerialPutChar(' ');
            // rip (16 hex digits)
            {
                uint64_t v = s.rip;
                for (int i = 15; i >= 0; --i) {
                    SerialPutChar(kHexDigits[(v >> (i * 4)) & 0xF]);
                }
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
        // Sleep until profiling completes (flush requested)
        while (!g_profilerFlushReq) {
            Process* self = ProcessCurrent();
            if (self) {
                self->wakeupTick = g_lapicTickCount + 50;
                SchedulerBlock(self);
            }
        }

        // Count dropped samples
        uint32_t cpuCount = SmpGetCpuCount();
        uint32_t totalDropped = 0;
        for (uint32_t c = 0; c < cpuCount; c++) {
            totalDropped += g_cpuBuf[c].dropped;
        }

        // Write all samples to serial (no VFS contention!)
        SerialPrintf("PROF_BEGIN %u %lu\n", cpuCount, g_profilerStartTick);

        uint32_t written = DrainToSerial();

        SerialPrintf("PROF_END %u %u\n", written, totalDropped);
        SerialPrintf("PROFILER: done — %u samples, %u dropped\n", written, totalDropped);

        g_profilerFlushReq = false;

        // Reset buffers for next run
        for (uint32_t c = 0; c < cpuCount; c++) {
            g_cpuBuf[c].writeIdx = 0;
            g_cpuBuf[c].readIdx  = 0;
            g_cpuBuf[c].dropped  = 0;
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

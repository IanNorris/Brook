// profiler.cpp — Sampling profiler implementation
//
// See profiler.h for overview.  The hot path (ProfilerSample) runs in ISR
// context on every CPU and must be lock-free and SSE-free.
//
// Design: per-CPU ring buffers accumulate events during profiling.
// When profiling stops, the drain thread dumps ALL events to the serial
// port in a parseable text format:
//   PROF_BEGIN <cpuCount> <startTick>
//   P  <tick> <pid_hex> <cpu> <flags> <rip_hex>   (sample)
//   CS <tick> <cpu> <old_pid_hex> <new_pid_hex>   (context switch)
//   ...
//   PROF_END <totalSamples> <dropped>
// A host-side script (profiler_to_speedscope.py) extracts these lines from
// the serial log and converts to Speedscope JSON.

#include "profiler.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
#include "serial.h"
#include "serial_writer.h"
#include "vfs.h"
#include "memory/heap.h"

// LAPIC tick counter (defined in apic.cpp).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

namespace brook {

// ---------------------------------------------------------------------------
// Event record — either a sample (P) or context-switch (CS)
// ---------------------------------------------------------------------------

enum class ProfileEventType : uint8_t { Sample = 0, ContextSwitch = 1 };

static constexpr uint32_t MAX_STACK_DEPTH = 8;

struct ProfileSample {
    ProfileEventType type;
    uint32_t tick;                     // relative to profiler start
    uint16_t pid;
    uint8_t  cpu;
    uint8_t  flags;                    // bit 0: ring (0=kernel, 1=user)
    uint64_t rip[MAX_STACK_DEPTH];     // [0] = leaf, [1..] = callers via RBP chain
    uint8_t  depth;                    // number of valid frames
    // Used only for ContextSwitch events:
    uint16_t newPid;
};

// ---------------------------------------------------------------------------
// Per-CPU lock-free ring buffer (single-producer ISR, single-consumer thread)
// ---------------------------------------------------------------------------
// Each sample is ~80 bytes with 8 stack frames.
// 4096 samples per CPU (~2.5 MB total), drained once at end of profiling.
// Why not continuous drain: DrainToSerial holds the serial lock while
// busy-waiting on the UART TX FIFO (115200 baud = ~87 µs/byte).  With CS
// events filling up at hundreds/second the drain takes ~8 s per cycle,
// starving serial_writer and all direct SerialPrintf callers for ~98% of the
// profiling window.  Accept ring-buffer overflow instead; the last
// SAMPLES_PER_CPU events per CPU are always preserved.

static constexpr uint32_t SAMPLES_PER_CPU = 4096;

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
    s.type  = ProfileEventType::Sample;
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
// Context-switch hook — emits a CS event into the calling CPU's ring buffer
// ---------------------------------------------------------------------------

void ProfilerContextSwitch(uint16_t oldPid, uint16_t newPid)
{
    if (!g_profilerEnabled) return;
    if (oldPid == newPid) return; // no actual switch (e.g. same idle process)

    uint32_t cpu = SmpCurrentCpuIndex();
    if (cpu >= MAX_PROFILER_CPUS) return;

    PerCpuBuffer& buf = g_cpuBuf[cpu];

    uint32_t wi = buf.writeIdx;
    uint32_t nextWi = (wi + 1) % SAMPLES_PER_CPU;
    if (nextWi == buf.readIdx) {
        buf.dropped++;
        return;
    }

    uint64_t now = g_lapicTickCount;

    ProfileSample& s = buf.samples[wi];
    s.type   = ProfileEventType::ContextSwitch;
    s.tick   = static_cast<uint32_t>(now - g_profilerStartTick);
    s.cpu    = static_cast<uint8_t>(cpu);
    s.pid    = oldPid;
    s.newPid = newPid;
    s.depth  = 0;

    __atomic_store_n(&buf.writeIdx, nextWi, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Drain samples to serial in parseable text format
// ---------------------------------------------------------------------------
// Output format (one line per sample, stack frames separated by semicolons):
//   P <tick_dec> <pid_hex> <cpu> <flags> <rip0_hex>;<rip1_hex>;...
// Delimited by PROF_BEGIN / PROF_END markers.

// ---------------------------------------------------------------------------
// Drain samples — format each event to a stack buffer, enqueue via
// SerialWriterEnqueue.  No serial lock held, no UART busy-wait here.
// ---------------------------------------------------------------------------

// Format helpers that write into a caller-supplied char buffer.
static uint32_t AppendHexDigit(char* buf, uint32_t pos, uint8_t v)
{
    buf[pos] = kHexDigits[v & 0xF];
    return pos + 1;
}

static uint32_t AppendHex4(char* buf, uint32_t pos, uint16_t v)
{
    pos = AppendHexDigit(buf, pos, (v >> 12) & 0xF);
    pos = AppendHexDigit(buf, pos, (v >>  8) & 0xF);
    pos = AppendHexDigit(buf, pos, (v >>  4) & 0xF);
    pos = AppendHexDigit(buf, pos, (v >>  0) & 0xF);
    return pos;
}

static uint32_t AppendHex16(char* buf, uint32_t pos, uint64_t v)
{
    for (int i = 15; i >= 0; --i)
        pos = AppendHexDigit(buf, pos, (v >> (i * 4)) & 0xF);
    return pos;
}

static uint32_t AppendDec(char* buf, uint32_t pos, uint32_t v)
{
    char tmp[11]; int ti = 10; tmp[ti] = '\0';
    if (v == 0) { tmp[--ti] = '0'; }
    else { while (v > 0) { tmp[--ti] = '0' + (v % 10); v /= 10; } }
    for (const char* p = &tmp[ti]; *p; ++p)
        buf[pos++] = *p;
    return pos;
}

// Format one ProfileSample into `buf` (must be ≥ 300 bytes).
// Returns number of characters written (no NUL terminator).
static uint32_t FormatEvent(const ProfileSample& s, char* buf)
{
    uint32_t p = 0;

    if (s.type == ProfileEventType::ContextSwitch) {
        // CS <tick> <cpu> <old_pid_hex> <new_pid_hex>
        buf[p++] = 'C'; buf[p++] = 'S'; buf[p++] = ' ';
        p = AppendDec(buf, p, s.tick);
        buf[p++] = ' ';
        buf[p++] = '0' + s.cpu;
        buf[p++] = ' ';
        p = AppendHex4(buf, p, s.pid);
        buf[p++] = ' ';
        p = AppendHex4(buf, p, s.newPid);
        buf[p++] = '\n';
    } else {
        // P <tick> <pid_hex> <cpu> <flags> <rip0>;...;<ripN>
        buf[p++] = 'P'; buf[p++] = ' ';
        p = AppendDec(buf, p, s.tick);
        buf[p++] = ' ';
        p = AppendHex4(buf, p, s.pid);
        buf[p++] = ' ';
        buf[p++] = '0' + s.cpu;
        buf[p++] = ' ';
        buf[p++] = '0' + s.flags;
        buf[p++] = ' ';
        for (uint8_t d = 0; d < s.depth; ++d) {
            if (d > 0) buf[p++] = ';';
            p = AppendHex16(buf, p, s.rip[d]);
        }
        buf[p++] = '\n';
    }
    return p;
}

static uint32_t AppendStr(char* buf, uint32_t pos, const char* str)
{
    while (*str) buf[pos++] = *str++;
    return pos;
}

// Write all buffered samples to /boot/profile.txt on the FAT boot partition.
// Uses a 16 KB heap buffer to coalesce small writes.  After the run, extract
// the file with:
//   mcopy -i build/release/brook_disk.img ::profile.txt ./profile.txt
// then process with:
//   python3 scripts/profiler_to_speedscope.py profile.txt
static uint32_t WriteProfileToDisk()
{
    constexpr const char* kPath = "/boot/profile.txt";
    Vnode* f = VfsOpen(kPath, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        SerialPrintf("PROFILER: failed to create %s\n", kPath);
        return 0;
    }

    constexpr uint32_t kBufSize = 16384;
    char* wbuf = static_cast<char*>(kmalloc(kBufSize));
    if (!wbuf) {
        SerialPrintf("PROFILER: kmalloc failed for write buffer\n");
        VfsClose(f);
        return 0;
    }

    uint64_t off    = 0;
    uint32_t wpos   = 0;
    uint32_t cpuCount = SmpGetCpuCount();

    // Flush write buffer to disk.
    auto flush = [&]() {
        if (wpos > 0) {
            VfsWrite(f, wbuf, wpos, &off);
            wpos = 0;
        }
    };

    // Append raw bytes to the write buffer, flushing as needed.
    auto append = [&](const char* src, uint32_t len) {
        while (len > 0) {
            uint32_t avail = kBufSize - wpos;
            if (avail == 0) { flush(); avail = kBufSize; }
            uint32_t n = (len < avail) ? len : avail;
            __builtin_memcpy(wbuf + wpos, src, n);
            wpos += n;
            src  += n;
            len  -= n;
        }
    };

    // PROF_BEGIN <cpuCount> <startTick>
    {
        char hdr[80]; uint32_t p = 0;
        p = AppendStr(hdr, p, "PROF_BEGIN ");
        p = AppendDec(hdr, p, cpuCount);
        hdr[p++] = ' ';
        p = AppendDec(hdr, p, static_cast<uint32_t>(g_profilerStartTick));
        hdr[p++] = '\n';
        append(hdr, p);
    }

    uint32_t totalWritten = 0;
    char lineBuf[300];

    for (uint32_t c = 0; c < cpuCount; c++) {
        PerCpuBuffer& buf = g_cpuBuf[c];
        uint32_t ri = buf.readIdx;
        uint32_t wi = __atomic_load_n(&buf.writeIdx, __ATOMIC_ACQUIRE);
        while (ri != wi) {
            uint32_t len = FormatEvent(buf.samples[ri], lineBuf);
            append(lineBuf, len);
            ri = (ri + 1) % SAMPLES_PER_CPU;
            totalWritten++;
        }
        buf.readIdx = ri;
    }

    // PROF_END <samples> <dropped>
    {
        uint32_t dropped = 0;
        for (uint32_t c = 0; c < cpuCount; c++) dropped += g_cpuBuf[c].dropped;

        char ftr[80]; uint32_t p = 0;
        p = AppendStr(ftr, p, "PROF_END ");
        p = AppendDec(ftr, p, totalWritten);
        ftr[p++] = ' ';
        p = AppendDec(ftr, p, dropped);
        ftr[p++] = '\n';
        append(ftr, p);
    }

    flush();
    kfree(wbuf);
    VfsClose(f);
    return totalWritten;
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
            // Collect into ring buffers silently while profiling runs.
            // No serial output during the recording window so kernel debug
            // messages (sys_socket, tcp:, etc.) can flow through freely.
            uint32_t cpuCount = SmpGetCpuCount();

            uint32_t elapsed = 0;
            while (g_profilerEnabled) {
                Process* self = ProcessCurrent();
                if (self) {
                    self->wakeupTick = g_lapicTickCount + 1000; // 1 s
                    SchedulerBlock(self);
                }
                elapsed++;
                if (elapsed % 10 == 0)
                    SerialPrintf("PROFILER: recording (%u s)\n", elapsed);
            }

            // Profiling ended — write everything to disk (fast, no baud-rate
            // limit).  Extract after QEMU exits with:
            //   mcopy -i build/release/brook_disk.img ::profile.txt ./profile.txt
            SerialPrintf("PROFILER: writing to /boot/profile.txt...\n");
            uint32_t totalWritten = WriteProfileToDisk();

            uint32_t totalDropped = 0;
            for (uint32_t c = 0; c < cpuCount; c++)
                totalDropped += g_cpuBuf[c].dropped;

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

bool ProfilerIsRunning()
{
    return g_profilerEnabled;
}

} // namespace brook

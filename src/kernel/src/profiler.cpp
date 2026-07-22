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
#include "vfs.h"
#include "rtc.h"
#include "memory/heap.h"
#include "panic_probe.h"   // PanicSafeReadU64 — fault-safe frame-pointer walk
#include "module.h"        // ModuleSnapshot — emit module bases for symbolication

// LAPIC tick counter (defined in apic.cpp).
namespace brook { extern volatile uint64_t g_lapicTickCount; }

namespace brook {

// ---------------------------------------------------------------------------
// Event record — either a sample (P) or context-switch (CS)
// ---------------------------------------------------------------------------

enum class ProfileEventType : uint8_t { Sample = 0, ContextSwitch = 1 };

static constexpr uint32_t MAX_STACK_DEPTH = 16;

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
static volatile bool     g_profilerFlushing  = false; // true while drain/close in progress
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
        // Wake the profiler thread promptly via pendingWakeup — we can't call
        // SchedulerUnblock from ISR context (it acquires g_readyLock), but
        // CheckBlockedWakeups checks pendingWakeup on the next timer tick.
        if (g_profilerThread)
            __atomic_store_n(&g_profilerThread->pendingWakeup, 1, __ATOMIC_RELEASE);
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

    // Walk the frame-pointer chain. Reads go through PanicSafeReadU64 so a wild
    // or paged-out RBP can never fault the profiler ISR (its .panic_extable fixup
    // turns a fault into a clean failure, handled at the top of HandleExceptionFull
    // before any panic-state check). This lets us cross into user frames too: for a
    // ring-3 sample the interrupted process's CR3 is live here, so user stack pages
    // that are present are readable; absent pages simply truncate the walk.
    if (interruptedRbp != 0) {
        constexpr uint64_t KERNEL_BASE   = 0xffffffff80000000ULL;
        constexpr uint64_t USER_CANON_MAX = 0x0000800000000000ULL; // exclusive
        uint64_t rbp = interruptedRbp;
        // A frame address is plausible if it is either a kernel-half address or a
        // canonical low-half (user) address, and 8-byte aligned.
        auto plausible = [](uint64_t a) -> bool {
            if (a & 7) return false;
            return (a >= KERNEL_BASE) || (a != 0 && a < USER_CANON_MAX);
        };
        while (depth < MAX_STACK_DEPTH) {
            if (!plausible(rbp)) break;
            uint64_t savedRbp = 0, retAddr = 0;
            // frame layout: [rbp+0]=saved RBP, [rbp+8]=return address
            if (!PanicSafeReadU64(rbp, &savedRbp)) break;
            if (!PanicSafeReadU64(rbp + 8, &retAddr)) break;
            // Return address must be a plausible text pointer (kernel or user).
            if (!((retAddr >= KERNEL_BASE) || (retAddr != 0 && retAddr < USER_CANON_MAX)))
                break;
            s.rip[depth++] = retAddr;
            if (savedRbp <= rbp) break;   // stack grows down; prevent loops
            rbp = savedRbp;
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

    // Capture the outgoing process's kernel stack at the CS point. This is
    // the most useful piece of data when investigating why a PID is stuck:
    // the call chain shows which blocking primitive (KMutex, FutexWait,
    // SchedulerBlock, VirtioBlk poll, etc.) the process is yielding into.
    // Walks the same RBP frame chain as ProfilerSample's kernel path.
    uint64_t rbp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    uint64_t leafRip = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    s.rip[0] = leafRip;
    uint8_t depth = 1;

    constexpr uint64_t KERNEL_BASE = 0xffffffff80000000ULL;
    constexpr uint64_t KERNEL_END  = 0xffffffffffffffffULL;
    while (depth < MAX_STACK_DEPTH) {
        if (rbp < KERNEL_BASE || rbp >= KERNEL_END - 16 || (rbp & 7) != 0)
            break;
        const uint64_t* frame = reinterpret_cast<const uint64_t*>(rbp);
        uint64_t retAddr = frame[1];
        if (retAddr < KERNEL_BASE || retAddr >= KERNEL_END)
            break;
        s.rip[depth++] = retAddr;
        uint64_t nextRbp = frame[0];
        if (nextRbp <= rbp) break;
        rbp = nextRbp;
    }
    s.depth = depth;

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
        // CS <tick> <cpu> <old_pid_hex> <new_pid_hex> [<rip0>;<rip1>;...]
        // Stack frames are the OUTGOING process's kernel callstack at the
        // point where it yielded — invaluable for diagnosing softlocks
        // (shows which blocking primitive each process is parked on).
        buf[p++] = 'C'; buf[p++] = 'S'; buf[p++] = ' ';
        p = AppendDec(buf, p, s.tick);
        buf[p++] = ' ';
        buf[p++] = '0' + s.cpu;
        buf[p++] = ' ';
        p = AppendHex4(buf, p, s.pid);
        buf[p++] = ' ';
        p = AppendHex4(buf, p, s.newPid);
        if (s.depth > 0) {
            buf[p++] = ' ';
            for (uint8_t d = 0; d < s.depth; ++d) {
                if (d > 0) buf[p++] = ';';
                p = AppendHex16(buf, p, s.rip[d]);
            }
        }
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

// ---------------------------------------------------------------------------
// Incremental profile file writer
//
// Usage:
//   ProfileWriter pw;
//   if (ProfileWriterOpen(&pw)) {
//       while (recording) { ProfileWriterDrain(&pw); sleep(1s); }
//       ProfileWriterClose(&pw);
//   }
//
// Extract after QEMU exits (filenames are timestamped PROF_YYYYMMDD_HHMMSS.TXT):
//   mcopy -i build/release/brook_disk.img '::PROF_*.TXT' ./
// then:
//   python3 scripts/profiler_to_speedscope.py PROF_20260511_143022.TXT
// ---------------------------------------------------------------------------

static constexpr uint32_t kProfBufSize = 16384;

// Flush + fsync the profile file every ~5 drain cycles (~5 s).  This persists
// the FatFS directory-entry size so an abrupt VM poweroff before ProfilerStop
// still yields a valid, parseable file.  Without it the sample sectors are on
// disk but the dirent reads 0 bytes (FatFS only updates the dirent size on
// f_sync/f_close).  Coarser than the old per-drain flush (commit a4fbce6) so
// steady-state recordings keep most of that commit's I/O reduction.
static constexpr uint32_t kProfSyncDrains = 5;

// Generate a timestamped profile path: /boot/PROF_YYYYMMDD_HHMMSS.TXT
// Falls back to /boot/PROFILE.TXT if RTC is unavailable.
static void BuildProfilePath(char* out, uint32_t outLen)
{
    uint64_t epoch = RtcNow();
    if (epoch == 0 || outLen < 40)
    {
        // Fallback
        const char* fb = "/boot/profile.txt";
        uint32_t i = 0;
        while (fb[i] && i + 1 < outLen) { out[i] = fb[i]; i++; }
        out[i] = '\0';
        return;
    }

    // Break epoch into date/time components
    uint64_t rem = epoch;
    uint32_t sec  = static_cast<uint32_t>(rem % 60); rem /= 60;
    uint32_t min  = static_cast<uint32_t>(rem % 60); rem /= 60;
    uint32_t hr   = static_cast<uint32_t>(rem % 24); rem /= 24;
    uint64_t days = rem;
    uint32_t yr   = 1970;
    while (true)
    {
        bool leap = (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0));
        uint32_t diy = leap ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        yr++;
    }
    static const uint32_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0));
    uint32_t mon = 1;
    for (uint32_t m = 0; m < 12; m++)
    {
        uint32_t d = dpm[m];
        if (m == 1 && leap) d = 29;
        if (days < d) { mon = m + 1; break; }
        days -= d;
    }
    uint32_t day = static_cast<uint32_t>(days) + 1;

    // Format: /boot/PROF_YYYYMMDD_HHMMSS.TXT
    auto d2 = [](char* p, uint32_t v) { p[0] = '0' + (v / 10); p[1] = '0' + (v % 10); };
    auto d4 = [](char* p, uint32_t v) {
        p[0] = '0' + (v / 1000) % 10;
        p[1] = '0' + (v / 100) % 10;
        p[2] = '0' + (v / 10) % 10;
        p[3] = '0' + v % 10;
    };
    //           /boot/PROF_YYYYMMDD_HHMMSS.TXT
    const char prefix[] = "/boot/PROF_";
    uint32_t i = 0;
    for (const char* p = prefix; *p; p++) out[i++] = *p;
    d4(out + i, yr);     i += 4;
    d2(out + i, mon);    i += 2;
    d2(out + i, day);    i += 2;
    out[i++] = '_';
    d2(out + i, hr);     i += 2;
    d2(out + i, min);    i += 2;
    d2(out + i, sec);    i += 2;
    const char suffix[] = ".TXT";
    for (const char* p = suffix; *p; p++) out[i++] = *p;
    out[i] = '\0';
}

struct ProfileWriter {
    Vnode*   file;
    char*    buf;
    uint64_t fileOff;
    uint32_t bufPos;
    uint32_t written;   // total sample lines written so far
    bool     writeError; // set once VfsWrite short-writes (e.g. disk full)
};

static void ProfileWriterFlush(ProfileWriter& pw)
{
    if (pw.bufPos > 0) {
        int wrote = VfsWrite(pw.file, pw.buf, pw.bufPos, &pw.fileOff);
        // A short write means the backing store rejected data — almost always a
        // full /boot disk. Without this the profiler would silently produce a
        // 0-byte (or truncated) PROF file yet still print "done", which is
        // exactly what masked a full-disk condition. Warn once, loudly.
        if (wrote != static_cast<int>(pw.bufPos) && !pw.writeError) {
            pw.writeError = true;
            SerialPrintf("PROFILER: WRITE FAILED (wrote %d of %u bytes) — profile "
                         "will be truncated/empty. Is /boot full?\n",
                         wrote, pw.bufPos);
        }
        pw.bufPos = 0;
    }
}

static void ProfileWriterAppend(ProfileWriter& pw, const char* src, uint32_t len)
{
    while (len > 0) {
        uint32_t avail = kProfBufSize - pw.bufPos;
        if (avail == 0) { ProfileWriterFlush(pw); avail = kProfBufSize; }
        uint32_t n = len < avail ? len : avail;
        __builtin_memcpy(pw.buf + pw.bufPos, src, n);
        pw.bufPos += n;
        src += n;
        len -= n;
    }
}

// Open the profile file and write the PROF_BEGIN header.
static bool ProfileWriterOpen(ProfileWriter& pw)
{
    char profPath[64];
    BuildProfilePath(profPath, sizeof(profPath));
    pw.file     = VfsOpen(profPath, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    pw.buf      = nullptr;
    pw.fileOff  = 0;
    pw.bufPos   = 0;
    pw.written  = 0;
    pw.writeError = false;

    if (!pw.file) {
        SerialPrintf("PROFILER: failed to create %s\n", profPath);
        return false;
    }
    SerialPrintf("PROFILER: writing to %s\n", profPath);

    pw.buf = static_cast<char*>(kmalloc(kProfBufSize));
    if (!pw.buf) {
        SerialPrintf("PROFILER: kmalloc failed for write buffer\n");
        VfsClose(pw.file);
        pw.file = nullptr;
        return false;
    }

    uint32_t cpuCount = SmpGetCpuCount();
    char hdr[80]; uint32_t p = 0;
    p = AppendStr(hdr, p, "PROF_BEGIN ");
    p = AppendDec(hdr, p, cpuCount);
    hdr[p++] = ' ';
    p = AppendDec(hdr, p, static_cast<uint32_t>(g_profilerStartTick));
    hdr[p++] = '\n';
    ProfileWriterAppend(pw, hdr, p);

    // BRO profiler: emit the module map so the host resolver can attribute
    // module-range RIPs (kernel .mod code loaded at dynamic VMM bases) to real
    // symbols. Format, one per active module:
    //   PROF_MOD <baseVirt_hex> <sizeBytes_hex> <name>
    // Emitted once, right after PROF_BEGIN, capturing modules already loaded at
    // profile start (load/unload during a capture is rare — boot loads them all).
    uint32_t modSlots = ModuleMaxSlots();
    for (uint32_t i = 0; i < modSlots; i++) {
        ModuleSnapshot ms = ModuleSnapshotAt(i);
        if (!ms.active || !ms.name || ms.baseVirt == 0) continue;
        char mline[128]; uint32_t mp = 0;
        mp = AppendStr(mline, mp, "PROF_MOD ");
        mp = AppendHex16(mline, mp, ms.baseVirt);
        mline[mp++] = ' ';
        mp = AppendHex16(mline, mp, ms.sizeBytes);
        mline[mp++] = ' ';
        for (const char* c = ms.name; *c && mp < (uint32_t)sizeof(mline) - 2; ++c)
            mline[mp++] = *c;
        mline[mp++] = '\n';
        ProfileWriterAppend(pw, mline, mp);
    }
    return true;
}

// Drain all CPU ring buffers into the open file. Safe to call repeatedly.
static void ProfileWriterDrain(ProfileWriter& pw)
{
    if (!pw.file) return;
    uint32_t cpuCount = SmpGetCpuCount();
    char lineBuf[300];
    for (uint32_t c = 0; c < cpuCount; c++) {
        PerCpuBuffer& buf = g_cpuBuf[c];
        uint32_t ri = buf.readIdx;
        uint32_t wi = __atomic_load_n(&buf.writeIdx, __ATOMIC_ACQUIRE);
        while (ri != wi) {
            uint32_t len = FormatEvent(buf.samples[ri], lineBuf);
            ProfileWriterAppend(pw, lineBuf, len);
            ri = (ri + 1) % SAMPLES_PER_CPU;
            pw.written++;
        }
        buf.readIdx = ri;
    }
    // Don't force-flush here — let the write buffer fill naturally.
    // ProfileWriterAppend flushes at 16KB boundaries; forcing a flush
    // after every drain caused a virtio-blk write every drain cycle
    // (~16% of a CPU on steady-state workloads like Q2).  Durability is
    // handled separately by ProfileWriterSync on a coarser cadence.
}

// Flush the in-memory buffer to the file and fsync it, persisting both the
// sample data and the on-disk directory-entry size.  Called periodically so a
// long indefinite capture survives an abrupt poweroff before close.
static void ProfileWriterSync(ProfileWriter& pw)
{
    if (!pw.file) return;
    ProfileWriterFlush(pw);
    VfsFsync(pw.file);
}

// Write PROF_END, flush and close the file.
static void ProfileWriterClose(ProfileWriter& pw)
{
    if (!pw.file) return;

    uint32_t dropped = 0;
    uint32_t cpuCount = SmpGetCpuCount();
    for (uint32_t c = 0; c < cpuCount; c++) dropped += g_cpuBuf[c].dropped;

    char ftr[80]; uint32_t p = 0;
    p = AppendStr(ftr, p, "PROF_END ");
    p = AppendDec(ftr, p, pw.written);
    ftr[p++] = ' ';
    p = AppendDec(ftr, p, dropped);
    ftr[p++] = '\n';
    ProfileWriterAppend(pw, ftr, p);

    ProfileWriterFlush(pw);
    kfree(pw.buf);
    VfsClose(pw.file);
    pw.file = nullptr;
    pw.buf  = nullptr;
}

// ---------------------------------------------------------------------------
// Profiler kernel thread — waits for profiling to complete, dumps to serial
// ---------------------------------------------------------------------------

static void ProfilerThreadFn(void* /*arg*/)
{
    SerialPrintf("PROFILER: thread started\n");

    for (;;) {
        // Sleep indefinitely until profiling starts.  Previously this used a
        // 100-tick timed self-sleep to poll g_profilerEnabled, which exercised
        // CheckBlockedWakeups → SchedulerUnblock churn from boot onward.  That
        // pattern was implicated in an SMP wakeup hang where userspace
        // processes failed to dispatch (~36% of boots).  Now ProfilerStart()
        // explicitly unblocks us via SchedulerUnblock; ProfilerStop sets
        // pendingWakeup the same way.
        while (!g_profilerEnabled && !g_profilerFlushReq) {
            Process* self = ProcessCurrent();
            if (self) {
                self->wakeupTick = 0; // no timed wake — wait for explicit unblock
                SchedulerBlock(self);
            }
        }

        if (g_profilerEnabled) {
            // Open the profile file immediately so we can drain into it each
            // second.  This prevents the 4096-slot per-CPU ring buffers from
            // overflowing during long recordings.
            ProfileWriter pw;
            bool fileOk = ProfileWriterOpen(pw);

            uint32_t cpuCount = SmpGetCpuCount();
            uint32_t drainsSinceSync = 0;
            while (g_profilerEnabled) {
                Process* self = ProcessCurrent();
                if (self) {
                    self->wakeupTick = g_lapicTickCount + 1000; // 1 s
                    SchedulerBlock(self);
                }
                // Use real wall time for progress messages, not wake count
                uint32_t elapsed = static_cast<uint32_t>(
                    (g_lapicTickCount - g_profilerStartTick) / 1000);
                if (elapsed % 10 == 0 && elapsed > 0)
                    SerialPrintf("PROFILER: recording (%u s)\n", elapsed);

                // Drain ring buffers into the file every second so they
                // don't overflow during long recordings.
                if (fileOk) {
                    ProfileWriterDrain(pw);
                    // Persist the dirent periodically so an abrupt poweroff
                    // before ProfilerStop still leaves a valid file.
                    if (++drainsSinceSync >= kProfSyncDrains) {
                        ProfileWriterSync(pw);
                        drainsSinceSync = 0;
                    }
                }
            }

            // Final drain to catch any events that arrived after the last
            // 1 s tick.
            __atomic_store_n(&g_profilerFlushing, true, __ATOMIC_RELEASE);
            if (fileOk)
                ProfileWriterDrain(pw);

            uint32_t totalDropped = 0;
            for (uint32_t c = 0; c < cpuCount; c++)
                totalDropped += g_cpuBuf[c].dropped;

            if (fileOk) {
                ProfileWriterClose(pw);
                SerialPrintf("PROFILER: done — %u samples, %u dropped\n",
                             pw.written, totalDropped);
            } else {
                SerialPrintf("PROFILER: done — file open failed, %u dropped\n",
                             totalDropped);
            }

            g_profilerFlushReq = false;

            // Reset buffers for next run
            for (uint32_t c = 0; c < cpuCount; c++) {
                g_cpuBuf[c].writeIdx = 0;
                g_cpuBuf[c].readIdx  = 0;
                g_cpuBuf[c].dropped  = 0;
            }
            __atomic_store_n(&g_profilerFlushing, false, __ATOMIC_RELEASE);
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
    if (__atomic_load_n(&g_profilerFlushing, __ATOMIC_ACQUIRE)) {
        SerialPrintf("PROFILER: still flushing previous run, try again shortly\n");
        return;
    }

    g_profilerStartTick = g_lapicTickCount;
    g_profilerEndTick   = durationMs > 0 ? (g_profilerStartTick + durationMs) : 0;
    g_profilerFlushReq  = false;

    // Enable sampling (ISRs will start recording on next tick)
    __atomic_store_n(&g_profilerEnabled, true, __ATOMIC_RELEASE);

    // Wake the profiler thread (it sleeps indefinitely on the disabled flag)
    if (g_profilerThread)
        SchedulerUnblock(g_profilerThread);

    SerialPrintf("PROFILER: recording started (%u ms, %u CPUs)\n",
                 durationMs, SmpGetCpuCount());
}

void ProfilerStop()
{
    if (!g_profilerEnabled) return;
    g_profilerEnabled  = false;
    g_profilerFlushReq = true;
    if (g_profilerThread)
        SchedulerUnblock(g_profilerThread);
    SerialPrintf("PROFILER: stop requested\n");
}

bool ProfilerIsRunning()
{
    return g_profilerEnabled || __atomic_load_n(&g_profilerFlushing, __ATOMIC_ACQUIRE);
}

} // namespace brook

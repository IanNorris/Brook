// panic.cpp — compiled with -mgeneral-regs-only so it is safe to call from
// interrupt context.  No SSE/FPU registers are touched here.
#include "panic.h"
#include "serial.h"
#include "tty.h"
#include "panic_qr.h"
#include "panic_screen.h"
#include "compositor.h"
#include "smp.h"
#include "build_info.h"
#include "ksym_addrs.h"
#include "scheduler.h"
#include "process.h"
#include "gdt.h"

// ---- Register capture -------------------------------------------------------
struct PanicRegs {
    uint64_t rsp;
    uint64_t rip;
    uint64_t cr2;
    uint64_t cr3;
};

static void CapturePanicRegs(PanicRegs& r)
{
    __asm__ volatile(
        "movq %%rsp, %0\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, %1\n\t"
        "movq %%cr2, %%rax\n\t"
        "movq %%rax, %2\n\t"
        "movq %%cr3, %%rax\n\t"
        "movq %%rax, %3\n\t"
        "1:"
        : "=m"(r.rsp), "=m"(r.rip), "=m"(r.cr2), "=m"(r.cr3)
        :
        : "rax"
    );
}

// Full CPU state capture for QR panic code
static void CaptureFullRegs(brook::PanicCPURegs& r)
{
    __asm__ volatile(
        "movq %%rax, %0\n\t"
        "movq %%rbx, %1\n\t"
        "movq %%rcx, %2\n\t"
        "movq %%rdx, %3\n\t"
        "movq %%rsi, %4\n\t"
        "movq %%rdi, %5\n\t"
        : "=m"(r.rax), "=m"(r.rbx), "=m"(r.rcx),
          "=m"(r.rdx), "=m"(r.rsi), "=m"(r.rdi)
    );
    __asm__ volatile(
        "movq %%r8,  %0\n\t"
        "movq %%r9,  %1\n\t"
        "movq %%r10, %2\n\t"
        "movq %%r11, %3\n\t"
        "movq %%r12, %4\n\t"
        "movq %%r13, %5\n\t"
        "movq %%r14, %6\n\t"
        "movq %%r15, %7\n\t"
        : "=m"(r.r8), "=m"(r.r9), "=m"(r.r10), "=m"(r.r11),
          "=m"(r.r12), "=m"(r.r13), "=m"(r.r14), "=m"(r.r15)
    );
    __asm__ volatile(
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, %0\n\t"
        "movq %%rsp, %1\n\t"
        "movq %%rbp, %2\n\t"
        "pushfq\n\t"
        "popq %3\n\t"
        "1:"
        : "=m"(r.rip), "=m"(r.rsp), "=m"(r.rbp), "=m"(r.rflags)
        :
        : "rax"
    );
    __asm__ volatile(
        "movq %%cr0, %%rax\n\t"
        "movq %%rax, %0\n\t"
        "movq %%cr2, %%rax\n\t"
        "movq %%rax, %1\n\t"
        "movq %%cr3, %%rax\n\t"
        "movq %%rax, %2\n\t"
        "movq %%cr4, %%rax\n\t"
        "movq %%rax, %3\n\t"
        : "=m"(r.cr0), "=m"(r.cr2), "=m"(r.cr3), "=m"(r.cr4)
        :
        : "rax"
    );
    __asm__ volatile(
        "movw %%cs, %0\n\t"
        "movw %%ds, %1\n\t"
        "movw %%ss, %2\n\t"
        "movw %%es, %3\n\t"
        "movw %%fs, %4\n\t"
        "movw %%gs, %5\n\t"
        : "=m"(r.cs), "=m"(r.ds), "=m"(r.ss),
          "=m"(r.es), "=m"(r.fs), "=m"(r.gs)
    );
    r.reserved = 0;
}

// ---- Stack trace capture (RBP frame walking) --------------------------------
static void CaptureStackTrace(brook::PanicStackTrace& trace, uint64_t rbp, uint64_t rip)
{
    constexpr uint64_t KERNEL_BASE = 0xffffffff80000000ULL;
    constexpr uint64_t KERNEL_END  = 0xffffffffffffffffULL;

    trace.depth = 0;

    // Frame 0: the RIP at panic time
    if (rip >= KERNEL_BASE && rip < KERNEL_END)
        trace.rip[trace.depth++] = rip;

    // Walk the RBP chain for caller frames
    while (trace.depth < brook::PANIC_MAX_STACK_DEPTH && rbp != 0)
    {
        // Validate RBP is in kernel range and aligned
        if (rbp < KERNEL_BASE || rbp >= KERNEL_END - 16 || (rbp & 7) != 0)
            break;

        const uint64_t* frame = reinterpret_cast<const uint64_t*>(rbp);
        uint64_t retAddr = frame[1];
        if (retAddr < KERNEL_BASE || retAddr >= KERNEL_END)
            break;

        trace.rip[trace.depth++] = retAddr;

        uint64_t nextRbp = frame[0];
        if (nextRbp <= rbp) break; // prevent loops (stack grows down)
        rbp = nextRbp;
    }
}

// ---- Minimal hex printer (no va_list needed) --------------------------------
static void SerialPutHex64(uint64_t v)
{
    brook::SerialPuts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        int nib = static_cast<int>((v >> shift) & 0xF);
        brook::SerialPutChar(static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

// ---- Minimal printf fan-out (no va_copy — serial first, TTY second) ---------
// We use a simple char-buffer approach to avoid needing va_copy from
// -mgeneral-regs-only context (where the ABI for va_list is tricky).
static char g_panicBuf[512];

static int PanicFormatStr(char* buf, int cap, const char* fmt, __builtin_va_list args)
{
    int pos = 0;

    auto putc = [&](char c) {
        if (pos < cap - 1) buf[pos++] = c;
    };
    auto puts = [&](const char* s) {
        if (!s) s = "(null)";
        while (*s) putc(*s++);
    };
    auto putu = [&](unsigned long v) {
        if (v == 0) { putc('0'); return; }
        char tmp[20]; int i = 0;
        while (v) { tmp[i++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (i > 0) putc(tmp[--i]);
    };
    auto puth = [&](unsigned long v) {
        if (v == 0) { putc('0'); return; }
        char tmp[16]; int i = 0;
        while (v) {
            int n = static_cast<int>(v & 0xF);
            tmp[i++] = static_cast<char>(n < 10 ? '0' + n : 'a' + n - 10);
            v >>= 4;
        }
        while (i > 0) putc(tmp[--i]);
    };
    auto putp = [&](unsigned long v) {
        puts("0x");
        for (int shift = 60; shift >= 0; shift -= 4) {
            int n = static_cast<int>((v >> shift) & 0xF);
            putc(static_cast<char>(n < 10 ? '0' + n : 'a' + n - 10));
        }
    };

    while (*fmt) {
        if (*fmt != '%') { putc(*fmt++); continue; }
        ++fmt;
        if (!*fmt) break;
        switch (*fmt) {
        case 's': puts(__builtin_va_arg(args, const char*)); break;
        case 'd': { int v = __builtin_va_arg(args, int);
                    if (v < 0) { putc('-'); putu((unsigned long)-(long)v); }
                    else putu((unsigned long)v); break; }
        case 'u': putu((unsigned long)__builtin_va_arg(args, unsigned int)); break;
        case 'x': puth((unsigned long)__builtin_va_arg(args, unsigned int)); break;
        case 'l':
            ++fmt;
            if      (*fmt == 'u') putu(__builtin_va_arg(args, unsigned long));
            else if (*fmt == 'x') puth(__builtin_va_arg(args, unsigned long));
            else if (*fmt == 'd') { long v = __builtin_va_arg(args, long);
                if (v < 0) { putc('-'); putu((unsigned long)-v); }
                else putu((unsigned long)v); }
            else { putc('l'); putc(*fmt); }
            break;
        case 'p': putp((unsigned long)__builtin_va_arg(args, void*)); break;
        case 'c': putc(static_cast<char>(__builtin_va_arg(args, int))); break;
        case '%': putc('%'); break;
        default:  putc('%'); putc(*fmt); break;
        }
        ++fmt;
    }

    buf[pos] = '\0';
    return pos;
}

// ---- KernelPanic ------------------------------------------------------------

// Fill a PanicCpuList from current kernel state. Safe to call from the panicking
// CPU after the APs are halted: per-CPU process + CR3 come from existing tracked
// state, and the spin RIP comes from the NMI-captured halted state if available
// (PANIC_CPU_LIVE_RIP), otherwise the last-scheduled RIP.
uint32_t brook::PanicCaptureCpuList(brook::PanicCpuList* out)
{
    if (!out) return 0;
    uint32_t n = brook::SmpGetCpuCount();
    if (n > brook::PANIC_MAX_CPUS_DUMP) n = brook::PANIC_MAX_CPUS_DUMP;
    out->count = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        brook::PanicCpuEntry& e = out->entries[out->count];
        e.cpuIndex = static_cast<uint8_t>(i);
        e.flags = 0;
        e.pid = 0;
        e.rip = 0;
        e.cr3 = 0;

        const brook::CpuInfo* ci = brook::SmpGetCpu(i);
        if (ci)
        {
            if (ci->online) e.flags |= brook::PANIC_CPU_ONLINE;
            if (ci->isBsp)  e.flags |= brook::PANIC_CPU_BSP;
            e.cr3 = ci->currentCr3;
        }

        // Prefer the NMI-captured live spin RIP if the panic NMI handler recorded
        // it; otherwise fall back to the last-scheduled RIP of the CPU's process.
        // pid is resolved independently from the scheduler's per-CPU table (the
        // NMI recorder leaves hs->pid==0), so a live RIP still gets a real pid.
        const brook::CpuHaltedState* hs = brook::SmpGetHaltedState(i);
        bool liveRip = false;
        if (hs && hs->halted)
        {
            e.flags |= brook::PANIC_CPU_HALTED;
            if (hs->rip)
            {
                e.rip = hs->rip;
                e.flags |= brook::PANIC_CPU_LIVE_RIP;
                liveRip = true;
            }
        }
        {
            brook::Process* p = brook::SchedulerGetCpuProcess(i);
            uint64_t pv = reinterpret_cast<uint64_t>(p);
            bool plausible = pv >= 0xFFFF800000000000ULL && (pv & 0x7) == 0;
            if (plausible && p->magic == brook::PROCESS_MAGIC)
            {
                e.pid = p->pid;
                if (!liveRip)
                    e.rip = p->savedCtx.rip;
            }
        }
        out->count++;
    }
    return out->count;
}

// Re-entrance guard — if the panic handler itself faults (e.g. TTY rendering
// hits an unmapped page), we detect it and emit a minimal serial message + halt.
static volatile int g_panicNesting = 0;

__attribute__((noreturn)) extern "C" void KernelPanic(const char* fmt, ...)
{
    __asm__ volatile("cli");

    int depth = __atomic_add_fetch(&g_panicNesting, 1, __ATOMIC_SEQ_CST);
    if (depth > 1)
    {
        // Nested panic — the handler itself faulted.  Serial-only, minimal output.
        brook::SerialPuts("\n*** DOUBLE PANIC (handler faulted) ***\n");
        for (;;) { __asm__ volatile("hlt"); }
    }

    PanicRegs regs;
    CapturePanicRegs(regs);

    brook::PanicCPURegs fullRegs;
    CaptureFullRegs(fullRegs);

    // Capture stack trace via RBP chain
    brook::PanicStackTrace trace;
    uint64_t captureRbp;
    __asm__ volatile("movq %%rbp, %0" : "=r"(captureRbp));
    CaptureStackTrace(trace, captureRbp, regs.rip);

    // Format the message into a static buffer.
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    PanicFormatStr(g_panicBuf, static_cast<int>(sizeof(g_panicBuf)), fmt, args);
    __builtin_va_end(args);

    // -- Serial output FIRST (before SmpHaltAllAPs) --------------------------
    // Print diagnostic info to serial before halting other CPUs.  If the
    // NMI broadcast crashes (BRO-109), the panic reason is still captured.
    brook::SerialPuts("\n*** KERNEL PANIC ***\n");
    brook::SerialPuts("Brook OS "); brook::SerialPuts(brook::BuildDate());
    brook::SerialPuts(" ("); brook::SerialPuts(brook::BuildGitBranch());
    brook::SerialPuts("/"); brook::SerialPuts(brook::BuildGitHash());
    brook::SerialPuts(")\n");
    brook::SerialPuts(g_panicBuf);
    brook::SerialPuts("RIP "); SerialPutHex64(regs.rip);
    // Symbolicate RIP
    {
        const char* symName = nullptr;
        uint64_t symOff = 0;
        if (brook::KsymFindByAddr(regs.rip, &symName, &symOff))
        {
            brook::SerialPuts("  ");
            brook::SerialPuts(symName);
            brook::SerialPuts("+0x");
            SerialPutHex64(symOff);
        }
    }
    brook::SerialPuts("\nRSP "); SerialPutHex64(regs.rsp);
    brook::SerialPuts("\nCR2 "); SerialPutHex64(regs.cr2);
    brook::SerialPuts("  CR3 "); SerialPutHex64(regs.cr3);

    // Stack trace with symbols
    brook::SerialPuts("\nStack trace (");
    {
        char depthStr[4];
        int ds = 0;
        if (trace.depth >= 10) depthStr[ds++] = '0' + (trace.depth / 10);
        depthStr[ds++] = '0' + (trace.depth % 10);
        depthStr[ds] = '\0';
        brook::SerialPuts(depthStr);
    }
    brook::SerialPuts(" frames):\n");
    for (uint8_t i = 0; i < trace.depth; i++) {
        brook::SerialPuts("  [");
        char idxStr[4];
        int is = 0;
        if (i >= 10) idxStr[is++] = '0' + (i / 10);
        idxStr[is++] = '0' + (i % 10);
        idxStr[is] = '\0';
        brook::SerialPuts(idxStr);
        brook::SerialPuts("] ");
        SerialPutHex64(trace.rip[i]);
        // Symbolicate
        const char* symName = nullptr;
        uint64_t symOff = 0;
        if (brook::KsymFindByAddr(trace.rip[i], &symName, &symOff))
        {
            brook::SerialPuts("  ");
            brook::SerialPuts(symName);
        }
        brook::SerialPuts("\n");
    }
    brook::SerialPuts("System halted.\n");

    // Halt other CPUs AFTER serial output — if the NMI broadcast crashes,
    // we've already printed the full diagnostic to serial.
    brook::SmpHaltAllAPs();

    // Now that the APs are halted and the panic NMI handler has captured each
    // CPU's live spin RIP into g_haltedState, print a per-CPU section to serial
    // symbolized by the KERNEL's own ksym table (reliable even on a dirty build,
    // unlike the QR decoder which symbolizes against the on-disk ELF). This is
    // the BRO-176 reap-stall diagnostic: it reveals where each AP was ACTUALLY
    // executing (e.g. spinning on a lock) rather than the stale savedCtx.rip.
    {
        uint32_t nCpus = brook::SmpGetCpuCount();
        if (nCpus > brook::MAX_CPUS) nCpus = brook::MAX_CPUS;
        brook::SerialPuts("\n--- per-CPU live state (NMI-captured) ---\n");
        for (uint32_t c = 0; c < nCpus; ++c)
        {
            const brook::CpuHaltedState* hs = brook::SmpGetHaltedState(c);
            brook::Process* p = brook::SchedulerGetCpuProcess(c);
            brook::SerialPuts("  CPU");
            char cbuf[4]; int ci2 = 0;
            if (c >= 10) cbuf[ci2++] = '0' + (c / 10);
            cbuf[ci2++] = '0' + (c % 10);
            cbuf[ci2] = '\0';
            brook::SerialPuts(cbuf);
            uint64_t pv = reinterpret_cast<uint64_t>(p);
            bool pOk = pv >= 0xFFFF800000000000ULL && (pv & 0x7) == 0
                       && p->magic == brook::PROCESS_MAGIC;
            brook::SerialPuts(" pid=");
            SerialPutHex64(pOk ? p->pid : 0);
            if (hs && hs->halted && hs->rip)
            {
                brook::SerialPuts(" LIVE rip=");
                SerialPutHex64(hs->rip);
                const char* sym = nullptr; uint64_t off = 0;
                if (brook::KsymFindByAddr(hs->rip, &sym, &off))
                {
                    brook::SerialPuts("  ");
                    brook::SerialPuts(sym);
                }
            }
            else if (pOk)
            {
                brook::SerialPuts(" sched rip=");
                SerialPutHex64(p->savedCtx.rip);
                const char* sym = nullptr; uint64_t off = 0;
                if (brook::KsymFindByAddr(p->savedCtx.rip, &sym, &off))
                {
                    brook::SerialPuts("  ");
                    brook::SerialPuts(sym);
                }
            }
            else
            {
                brook::SerialPuts(" (idle/none)");
            }
            brook::SerialPuts("\n");
        }
    }

    // Stop the compositor so nothing overwrites the panic screen.
    brook::CompositorHalt();

    // -- Visual panic screen (if framebuffer is up) ----------------------------
    // Use the physical framebuffer directly — the compositor's backbuffer
    // won't be flushed since the compositor is halted.
    uint32_t physStride = 0;
    volatile uint32_t* physFb = brook::CompositorGetPhysFb(&physStride);
    uint32_t fbW = 0, fbH = 0;
    brook::CompositorGetPhysDims(&fbW, &fbH);
    if (physFb && fbW && fbH)
    {
        // Snapshot running processes (all CPUs halted, no lock needed)
        static brook::PanicProcessList procList;
        procList.count = 0;
        uint32_t nProcs = brook::PanicGetProcessCount();
        for (uint32_t i = 0; i < nProcs && procList.count < brook::PANIC_MAX_PROCESSES; i++)
        {
            brook::Process* p = brook::PanicGetProcess(i);
            if (!p) continue;
            auto& e = procList.entries[procList.count];
            // BRO-176/SIG2: VALIDATE before dereferencing. A freed-and-reused or
            // wild Process* in g_allProcesses (the kernel-corruption bug we're
            // hunting) would fault the panic handler itself if we read its fields
            // blind — turning a crash into a hung double-panic during QR build.
            // Range-check the pointer, then check magic; only then read fields.
            uint64_t pv = reinterpret_cast<uint64_t>(p);
            bool plausible = pv >= 0xFFFF800000000000ULL && (pv & 0x7) == 0;
            if (!plausible || p->magic != brook::PROCESS_MAGIC)
            {
                // Record a minimal corrupt-entry stub WITHOUT further deref; stash
                // the bad pointer in rip for forensics. This is SIG2 evidence.
                e.pid = 0xFFFF; e.state = 0xFF; e.cpu = 0xFF; e.rip = pv;
                e.name[0] = '?'; e.name[1] = '\0';
                e.tgid = 0; e.asLiveThreads = -1; e.refCount = 0;
                e.flags = brook::PANIC_PROC_MAGIC_BAD;
                procList.count++;
                continue;
            }
            e.pid   = p->pid;
            e.state = static_cast<uint8_t>(p->state);
            e.cpu   = (p->runningOnCpu >= 0) ? static_cast<uint8_t>(p->runningOnCpu) : 0xFF;
            e.rip   = p->savedCtx.rip;
            // Copy name (truncate if needed)
            for (uint32_t j = 0; j < brook::PANIC_PROCESS_NAME_LEN; j++)
                e.name[j] = (p->name[j]) ? p->name[j] : '\0';
            // Reap-gate fields
            e.tgid = p->tgid;
            bool leader = !p->isThread;
            e.asLiveThreads = leader
                ? static_cast<int16_t>(__atomic_load_n(&p->asLiveThreads, __ATOMIC_RELAXED))
                : static_cast<int16_t>(-1);
            e.refCount = static_cast<int16_t>(__atomic_load_n(&p->refCount, __ATOMIC_RELAXED));
            e.flags = 0;
            if (p->isThread)        e.flags |= brook::PANIC_PROC_IS_THREAD;
            if (p->reapable)        e.flags |= brook::PANIC_PROC_REAPABLE;
            if (p->isKernelThread)  e.flags |= brook::PANIC_PROC_IS_KTHREAD;
            procList.count++;
        }

        // Capture system metadata
        static brook::PanicSystemInfo sysInfo = {};
        {
            uint32_t cpuIdx = brook::SmpCurrentCpuIndex();
            sysInfo.cpuIndex = static_cast<uint8_t>(cpuIdx);
            sysInfo.cpuCount = static_cast<uint8_t>(brook::SmpGetCpuCount());

            // RDTSC for uptime approximation
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            sysInfo.tscTicks = (static_cast<uint64_t>(hi) << 32) | lo;

            // TSS RSP0 for faulting CPU
            auto* tss = GdtGetTss(cpuIdx);
            sysInfo.tssRsp0 = tss ? tss->rsp[0] : 0;

            // Copy short git hash
            const char* hash = brook::BuildGitHash();
            for (uint32_t j = 0; j < brook::PANIC_GIT_HASH_LEN - 1 && hash[j]; j++)
                sysInfo.gitHash[j] = hash[j];
            sysInfo.gitHash[brook::PANIC_GIT_HASH_LEN - 1] = '\0';

            // Copy branch name
            const char* branch = brook::BuildGitBranch();
            for (uint32_t j = 0; j < brook::PANIC_GIT_BRANCH_LEN - 1 && branch[j]; j++)
                sysInfo.gitBranch[j] = branch[j];
            sysInfo.gitBranch[brook::PANIC_GIT_BRANCH_LEN - 1] = '\0';
        }

        // Capture raw stack bytes from RSP
        static brook::PanicStackDump stackDump = {};
        {
            stackDump.rsp = regs.rsp;
            stackDump.length = 0;
            const uint8_t* rspPtr = reinterpret_cast<const uint8_t*>(regs.rsp);
            uint64_t rspAddr = regs.rsp;
            // Only read if RSP is in kernel-half canonical range
            if (rspAddr >= 0xFFFF800000000000ULL && rspAddr != 0)
            {
                uint16_t maxBytes = brook::PANIC_STACK_DUMP_BYTES;
                for (uint16_t i = 0; i < maxBytes; i++)
                {
                    stackDump.data[i] = rspPtr[i];
                    stackDump.length = i + 1;
                }
            }
        }

        uint32_t fbStride = physStride * 4; // pixel stride → byte stride
        static brook::PanicCpuList cpuList;
        brook::PanicCaptureCpuList(&cpuList);
        brook::PanicScreenInfo psi = {};
        psi.message   = g_panicBuf;
        psi.regs      = &fullRegs;
        psi.trace     = &trace;
        psi.excInfo   = nullptr;  // KernelPanic — no exception
        psi.procList  = &procList;
        psi.sysInfo   = &sysInfo;
        psi.stackDump = &stackDump;
        psi.cpuList   = &cpuList;
        psi.vector    = 0;
        psi.errorCode = 0;
        brook::PanicScreenRender(const_cast<uint32_t*>(physFb), fbW, fbH, fbStride, &psi);
    }

    // Spin forever (don't use hlt — it causes QEMU to exit when all CPUs halt)
    for (;;) { __asm__ volatile("pause"); }
}

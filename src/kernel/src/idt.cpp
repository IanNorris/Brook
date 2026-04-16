#include "idt.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"
#include "process.h"
#include "scheduler.h"
#include "apic.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"

using brook::SerialPrintf;
using brook::IoApicUnmaskIrq;
using brook::ApicSendEoi;

// ---- IDT storage ----
static IdtEntry      g_idt[256];
static IdtDescriptor g_idtDesc;

// ---- Full exception frame (matches exception_stubs.S push order) ----
// Assembly stubs for vectors 13/14 push all GPRs before calling
// HandleExceptionFull, allowing signal delivery to modify return registers.
struct FullExceptionFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t errorCode;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

// ---- Helper to fill one IDT entry ----
static void SetIdtEntry(uint8_t vector, void* handler, uint8_t ist = 0)
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(handler);
    g_idt[vector].offsetLow  = static_cast<uint16_t>(addr & 0xFFFF);
    g_idt[vector].selector   = GDT_KERNEL_CODE;
    g_idt[vector].ist        = ist;
    g_idt[vector].typeAttr   = 0x8E;
    g_idt[vector].offsetMid  = static_cast<uint16_t>((addr >> 16) & 0xFFFF);
    g_idt[vector].offsetHigh = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector]._reserved  = 0;
}

// ---- Exception name table ----
static const char* const g_excNames[32] = {
    "#DE Divide Error",            // 0
    "#DB Debug",                   // 1
    "NMI",                         // 2
    "#BP Breakpoint",              // 3
    "#OF Overflow",                // 4
    "#BR Bound Range Exceeded",    // 5
    "#UD Invalid Opcode",          // 6
    "#NM Device Not Available",    // 7
    "#DF Double Fault",            // 8
    "Coprocessor Segment Overrun", // 9
    "#TS Invalid TSS",             // 10
    "#NP Segment Not Present",     // 11
    "#SS Stack-Segment Fault",     // 12
    "#GP General Protection",      // 13
    "#PF Page Fault",              // 14
    "Reserved (15)",               // 15
    "#MF x87 FPU Exception",       // 16
    "#AC Alignment Check",         // 17
    "#MC Machine Check",           // 18
    "#XF SIMD Exception",          // 19
    "#VE Virtualization Exception",// 20
    "#CP Control Protection",      // 21
    "Reserved (22)",               // 22
    "Reserved (23)",               // 23
    "Reserved (24)",               // 24
    "Reserved (25)",               // 25
    "Reserved (26)",               // 26
    "Reserved (27)",               // 27
    "#HV Hypervisor Injection",    // 28
    "#VC VMM Communication",       // 29
    "#SX Security Exception",      // 30
    "Reserved (31)",               // 31
};

// ---- Common exception dispatch ----
// Called from the __attribute__((interrupt)) stubs below.
// KernelPanic is [[noreturn]] so this never returns.

// ---------------------------------------------------------------------------
// Lockless serial output for exception handlers.
// These bypass the serial spinlock entirely — during a fault we cannot risk
// spinning on a lock that another CPU (or this CPU's interrupted code) holds.
// Output may interleave with other CPUs but will always make progress.
// ---------------------------------------------------------------------------

static inline void ExcOutb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t ExcInb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void ExcPutCharRaw(char c)
{
    if (c == '\n') {
        while ((ExcInb(0x3FD) & 0x20) == 0) {}
        ExcOutb(0x3F8, '\r');
    }
    while ((ExcInb(0x3FD) & 0x20) == 0) {}
    ExcOutb(0x3F8, static_cast<uint8_t>(c));
}

static void ExcPutsRaw(const char* s)
{
    if (!s) return;
    while (*s) ExcPutCharRaw(*s++);
}

static void ExcPutHex(uint64_t v)
{
    ExcPutsRaw("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        int nib = static_cast<int>((v >> shift) & 0xF);
        ExcPutCharRaw(static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

// Frame-pointer-based stack walk.  RBP chain: [RBP] → saved_RBP, [RBP+8] → return_RIP.
struct StackFrame {
    StackFrame* rbp;
    uint64_t    rip;
};

static void ExcStackWalk(uint64_t rbp, int maxFrames, const char* tag)
{
    ExcPutsRaw(tag); ExcPutsRaw("  --- stack trace ---\n");
    auto* fp = reinterpret_cast<StackFrame*>(rbp);
    StackFrame* prev = nullptr;

    for (int i = 0; i < maxFrames; ++i)
    {
        if (fp == nullptr || fp == prev || fp->rip == 0)
            break;

        // Only follow pointers in kernel space; user-mode RBP values
        // could reference corrupted page tables and trigger nested faults.
        auto addr = reinterpret_cast<uint64_t>(fp);
        if (addr < 0xFFFF800000000000ULL)
            break;

        ExcPutsRaw(tag); ExcPutsRaw("  #");
        ExcPutCharRaw(static_cast<char>('0' + (i / 10) % 10));
        ExcPutCharRaw(static_cast<char>('0' + i % 10));
        ExcPutsRaw("  ");
        ExcPutHex(fp->rip);
        ExcPutsRaw("\n");

        prev = fp;
        fp = fp->rbp;
    }
    ExcPutsRaw(tag); ExcPutsRaw("  --- end trace ---\n");
}

static void HandleException(uint8_t vector, InterruptFrame* frame, uint64_t errorCode, bool hasErrorCode, bool swapgsDone = false)
{
    __asm__ volatile("cli");

    // The __attribute__((interrupt)) stubs do NOT perform SWAPGS.
    // If we entered from user mode (ring 3), GS base is the user's TLS pointer.
    // We must swap to get the kernel per-CPU pointer so that later code
    // (context_switch, etc.) leaves GS in a consistent state.
    // Skip if the assembly stub already did SWAPGS.
    bool fromUser = (frame->cs & 3) != 0;
    if (fromUser && !swapgsDone)
        __asm__ volatile("swapgs");

    // Per-CPU nesting guard — uses LAPIC ID to index, avoiding any lock.
    // 64 entries covers typical SMP configs.  Falls back to entry 0 for
    // LAPIC IDs >= 64 (safe — two CPUs sharing an entry just means a
    // slightly wider nesting window, still no deadlock).
    static volatile int excDepthPerCpu[64] = {};
    uint32_t lapicId;
    __asm__ volatile("mov $1, %%eax; cpuid; shr $24, %%ebx; mov %%ebx, %0"
                     : "=r"(lapicId) : : "eax","ebx","ecx","edx");
    uint32_t cpuSlot = (lapicId < 64) ? lapicId : 0;

    if (excDepthPerCpu[cpuSlot] > 0)
    {
        ExcPutsRaw("\n=== NESTED EXCEPTION on CPU ");
        ExcPutCharRaw(static_cast<char>('0' + cpuSlot % 10));
        ExcPutsRaw(" — halting ===\n");
        ExcPutsRaw("  RIP   "); ExcPutHex(frame->ip); ExcPutsRaw("\n");
        ExcPutsRaw("  RSP   "); ExcPutHex(frame->sp); ExcPutsRaw("\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    ++excDepthPerCpu[cpuSlot];

    // Build a CPU tag prefix like "[C3] " so interleaved lockless output
    // from multiple CPUs can be disambiguated.
    char cpuTag[6] = {'[','C','0',']',' ','\0'};
    cpuTag[2] = static_cast<char>('0' + cpuSlot % 10);

    uint64_t cr2 = 0;
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));

    uint64_t rbp = 0;
    __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));

    const char* name = (vector < 32) ? g_excNames[vector] : "Unknown";

    // Direct serial dump — guaranteed to produce readable output regardless
    // of va_list ABI issues or format buffer corruption.
    // Every line is prefixed with [CN] so interleaved output from multiple
    // CPUs is still parseable.
    ExcPutsRaw("\n"); ExcPutsRaw(cpuTag); ExcPutsRaw("=== EXCEPTION ===\n");
    ExcPutsRaw(cpuTag); ExcPutsRaw("Vector: ");
    {
        char vbuf[4] = {'0','0','0','\0'};
        vbuf[0] = static_cast<char>('0' + (vector / 100) % 10);
        vbuf[1] = static_cast<char>('0' + (vector / 10) % 10);
        vbuf[2] = static_cast<char>('0' + vector % 10);
        ExcPutsRaw(vbuf);
    }
    ExcPutsRaw(" ("); ExcPutsRaw(name); ExcPutsRaw(")\n");

    // Print current process info if available.
    brook::Process* cur = brook::ProcessCurrent();
    if (cur) {
        ExcPutsRaw(cpuTag); ExcPutsRaw("  PID   ");
        {
            char pbuf[6];
            uint16_t pid = cur->pid;
            pbuf[0] = static_cast<char>('0' + (pid / 10000) % 10);
            pbuf[1] = static_cast<char>('0' + (pid / 1000) % 10);
            pbuf[2] = static_cast<char>('0' + (pid / 100) % 10);
            pbuf[3] = static_cast<char>('0' + (pid / 10) % 10);
            pbuf[4] = static_cast<char>('0' + pid % 10);
            pbuf[5] = '\0';
            // Skip leading zeros.
            const char* p = pbuf;
            while (*p == '0' && p[1] != '\0') ++p;
            ExcPutsRaw(p);
        }
        ExcPutsRaw(" ("); ExcPutsRaw(cur->name); ExcPutsRaw(")\n");
    }

    ExcPutsRaw(cpuTag); ExcPutsRaw("  RIP   "); ExcPutHex(frame->ip);    ExcPutsRaw("\n");
    ExcPutsRaw(cpuTag); ExcPutsRaw("  RSP   "); ExcPutHex(frame->sp);    ExcPutsRaw("\n");
    ExcPutsRaw(cpuTag); ExcPutsRaw("  CS    "); ExcPutHex(frame->cs);    ExcPutsRaw("\n");
    ExcPutsRaw(cpuTag); ExcPutsRaw("  SS    "); ExcPutHex(frame->ss);    ExcPutsRaw("\n");
    ExcPutsRaw(cpuTag); ExcPutsRaw("  FLAGS "); ExcPutHex(frame->flags); ExcPutsRaw("\n");
    ExcPutsRaw(cpuTag); ExcPutsRaw("  CR2   "); ExcPutHex(cr2);          ExcPutsRaw("\n");
    if (hasErrorCode) {
        ExcPutsRaw(cpuTag); ExcPutsRaw("  ERR   "); ExcPutHex(errorCode);
        if (vector == 14) {
            ExcPutsRaw(" [");
            ExcPutsRaw((errorCode & 1) ? "P " : "NP ");
            ExcPutsRaw((errorCode & 2) ? "W " : "R ");
            ExcPutsRaw((errorCode & 4) ? "U " : "K ");
            if (errorCode & 8) ExcPutsRaw("RSVD ");
            ExcPutsRaw("]");
        }
        ExcPutsRaw("\n");
    }

    // For page faults, walk the page table hierarchy to diagnose the mapping.
    // Uses the direct physical map (DIRECT_MAP_BASE) to read page tables safely.
    if (vector == 14)
    {
        static constexpr uint64_t DMAP_USR = 0xFFFF800000000000ULL;

        uint64_t cr3val;
        __asm__ volatile("movq %%cr3, %0" : "=r"(cr3val));
        cr3val &= 0x000FFFFFFFFFF000ULL;

        ExcPutsRaw(cpuTag); ExcPutsRaw("  --- page table walk ---\n");
        ExcPutsRaw(cpuTag); ExcPutsRaw("  CR3   "); ExcPutHex(cr3val); ExcPutsRaw("\n");

        uint64_t* pml4 = reinterpret_cast<uint64_t*>(DMAP_USR + cr3val);
        uint64_t pml4idx = (cr2 >> 39) & 0x1FF;
        uint64_t pml4e = pml4[pml4idx];
        ExcPutsRaw(cpuTag); ExcPutsRaw("  PML4["); ExcPutHex(pml4idx); ExcPutsRaw("] = ");
        ExcPutHex(pml4e); ExcPutsRaw("\n");

        if (pml4e & 1)
        {
            uint64_t* pdpt = reinterpret_cast<uint64_t*>(DMAP_USR + (pml4e & 0x000FFFFFFFFFF000ULL));
            uint64_t pdptidx = (cr2 >> 30) & 0x1FF;
            uint64_t pdpte = pdpt[pdptidx];
            ExcPutsRaw(cpuTag); ExcPutsRaw("  PDPT["); ExcPutHex(pdptidx); ExcPutsRaw("] = ");
            ExcPutHex(pdpte); ExcPutsRaw("\n");

            if (pdpte & 1)
            {
                uint64_t* pd = reinterpret_cast<uint64_t*>(DMAP_USR + (pdpte & 0x000FFFFFFFFFF000ULL));
                uint64_t pdidx = (cr2 >> 21) & 0x1FF;
                uint64_t pde = pd[pdidx];
                ExcPutsRaw(cpuTag); ExcPutsRaw("  PD["); ExcPutHex(pdidx); ExcPutsRaw("] = ");
                ExcPutHex(pde); ExcPutsRaw("\n");

                if ((pde & 1) && !(pde & (1ULL << 7)))
                {
                    uint64_t* pt = reinterpret_cast<uint64_t*>(DMAP_USR + (pde & 0x000FFFFFFFFFF000ULL));
                    uint64_t ptidx = (cr2 >> 12) & 0x1FF;
                    uint64_t pte = pt[ptidx];
                    ExcPutsRaw(cpuTag); ExcPutsRaw("  PT["); ExcPutHex(ptidx); ExcPutsRaw("] = ");
                    ExcPutHex(pte); ExcPutsRaw("\n");
                }
            }
            else
            {
                ExcPutsRaw(cpuTag); ExcPutsRaw("  PDPT entry NOT PRESENT\n");
                ExcPutsRaw(cpuTag); ExcPutsRaw("  PDPT page dump:\n");
                for (int d = 0; d < 8; ++d)
                {
                    ExcPutsRaw(cpuTag); ExcPutsRaw("    ["); ExcPutHex(static_cast<uint64_t>(d));
                    ExcPutsRaw("] = "); ExcPutHex(pdpt[d]);
                    ExcPutsRaw("\n");
                }
            }
        }
        ExcPutsRaw(cpuTag); ExcPutsRaw("  --- end walk ---\n");
    }

    // For user-mode faults, dump a few values from the user stack.
    if ((frame->cs & 3) == 3)
    {
        ExcPutsRaw(cpuTag); ExcPutsRaw("  --- user stack dump (RSP) ---\n");
        auto userRsp = frame->sp;
        static constexpr uint64_t DMAP_USR2 = 0xFFFF800000000000ULL;
        uint64_t cr3val;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3val));
        for (int i = 0; i < 8; ++i)
        {
            auto addr = userRsp + static_cast<uint64_t>(i) * 8;
            if (addr >= 0x800000000000ULL) break;

            uint64_t* l4 = reinterpret_cast<uint64_t*>(DMAP_USR2 + (cr3val & 0x000FFFFFFFFFF000ULL));
            uint64_t l4e = l4[(addr >> 39) & 0x1FF];
            if (!(l4e & 1)) { ExcPutsRaw(cpuTag); ExcPutsRaw("    (unmapped PML4)\n"); break; }
            uint64_t* l3 = reinterpret_cast<uint64_t*>(DMAP_USR2 + (l4e & 0x000FFFFFFFFFF000ULL));
            uint64_t l3e = l3[(addr >> 30) & 0x1FF];
            if (!(l3e & 1)) { ExcPutsRaw(cpuTag); ExcPutsRaw("    (unmapped PDPT)\n"); break; }
            uint64_t* l2 = reinterpret_cast<uint64_t*>(DMAP_USR2 + (l3e & 0x000FFFFFFFFFF000ULL));
            uint64_t l2e = l2[(addr >> 21) & 0x1FF];
            if (!(l2e & 1)) { ExcPutsRaw(cpuTag); ExcPutsRaw("    (unmapped PD)\n"); break; }
            uint64_t physPage;
            if (l2e & 0x80) {
                physPage = (l2e & 0x000FFFFFFFE00000ULL) + (addr & 0x1FFFFF);
            } else {
                uint64_t* l1 = reinterpret_cast<uint64_t*>(DMAP_USR2 + (l2e & 0x000FFFFFFFFFF000ULL));
                uint64_t l1e = l1[(addr >> 12) & 0x1FF];
                if (!(l1e & 1)) { ExcPutsRaw(cpuTag); ExcPutsRaw("    (unmapped PT)\n"); break; }
                physPage = (l1e & 0x000FFFFFFFFFF000ULL) + (addr & 0xFFF);
            }
            uint64_t val = *reinterpret_cast<uint64_t*>(DMAP_USR2 + physPage);

            ExcPutsRaw(cpuTag); ExcPutsRaw("    [RSP+");
            ExcPutHex(static_cast<uint64_t>(i * 8));
            ExcPutsRaw("] = ");
            ExcPutHex(val);
            ExcPutsRaw("\n");
        }
        ExcPutsRaw(cpuTag); ExcPutsRaw("  --- end user stack ---\n");
    }
    ExcStackWalk(rbp, 32, cpuTag);

    // Detect kernel stack guard page hits for #PF.
    if (vector == 14)
    {
        brook::Process* guardProc = brook::ProcessCurrent();
        if (guardProc && guardProc->kernelStackBase)
        {
            // Guard page is one page below kernelStackBase.
            uint64_t guardLow  = guardProc->kernelStackBase - 4096;
            // Guard page is one page above kernelStackTop.
            uint64_t guardHigh = guardProc->kernelStackBase + brook::KERNEL_STACK_SIZE;
            if (cr2 >= guardLow && cr2 < guardProc->kernelStackBase)
            {
                ExcPutsRaw(cpuTag); ExcPutsRaw("\n*** KERNEL STACK OVERFLOW (bottom guard page hit) ***\n");
            }
            else if (cr2 >= guardHigh && cr2 < guardHigh + 4096)
            {
                ExcPutsRaw(cpuTag); ExcPutsRaw("\n*** KERNEL STACK OVERFLOW (top guard page hit) ***\n");
            }
        }
    }

    // If the fault came from user mode (ring 3), kill the process and continue.
    // Kernel-mode faults are unrecoverable — halt.
    if ((frame->cs & 3) == 3)
    {
        ExcPutsRaw(cpuTag); ExcPutsRaw("=== KILLING PROCESS ===\n");
        --excDepthPerCpu[cpuSlot];
        __asm__ volatile("sti");
        brook::SchedulerExitCurrentProcess(-static_cast<int>(vector));
        // Never reached.
    }

    ExcPutsRaw(cpuTag); ExcPutsRaw("=== HALTED ===\n");

    // Halt here — kernel-mode exception is unrecoverable.
    // Use cli before hlt so timer interrupts don't wake us.
    for (;;) { __asm__ volatile("cli; hlt"); }
}

// ---------------------------------------------------------------------------
// HandleExceptionFull — called from assembly stubs (exception_stubs.S)
// for vectors 13 (#GP) and 14 (#PF).
//
// Has access to all GPRs via FullExceptionFrame, enabling signal delivery
// that redirects user-mode execution to a registered signal handler.
// ---------------------------------------------------------------------------

// Map exception vectors to Linux signal numbers.
static int ExceptionToSignal(uint8_t vector)
{
    switch (vector) {
    case  0: return 8;   // #DE → SIGFPE
    case  6: return 4;   // #UD → SIGILL
    case 13: return 11;  // #GP → SIGSEGV
    case 14: return 11;  // #PF → SIGSEGV
    default: return 11;  // default to SIGSEGV
    }
}

// SI_KERNEL code for synchronous exceptions
static constexpr int32_t SI_KERNEL = 0x80;
// SEGV_MAPERR / SEGV_ACCERR codes
static constexpr int32_t SEGV_MAPERR = 1;
static constexpr int32_t SEGV_ACCERR = 2;

extern "C" void HandleExceptionFull(FullExceptionFrame* ef, uint64_t vector)
{
    // The assembly stub has already done SWAPGS if needed.
    __asm__ volatile("cli");

    bool fromUser = (ef->cs & 3) != 0;

    // --- COW page fault handling (both kernel and user mode) ---
    // The kernel writes to user COW pages via sys_wait4, sys_read, etc.
    // These are kernel-mode writes (error code bit 2 = 0) to user-space
    // addresses that have COW protection. Handle them before the kernel
    // fault path to avoid a spurious panic.
    if (vector == 14)
    {
        uint64_t cr2cow = 0;
        __asm__ volatile("movq %%cr2, %0" : "=r"(cr2cow));

        bool pfPresent = (ef->errorCode & 1) != 0;
        bool pfWrite   = (ef->errorCode & 2) != 0;
        // COW applies to user-mapped pages regardless of CPL
        bool isUserAddr = (cr2cow < 0x0000800000000000ULL);

        brook::Process* cowProc = brook::ProcessCurrent();
        if (pfPresent && pfWrite && isUserAddr && cowProc)
        {
            using namespace brook;
            uint64_t* pte = VmmGetPte(cowProc->pageTable,
                                      VirtualAddress(cr2cow & ~0xFFFULL));
            if (pte && (*pte & PTE_COW_BIT))
            {
                static constexpr uint64_t PTE_PHYS_MASK = 0x000FFFFFFFFFF000ULL;
                PhysicalAddress oldPhys((*pte) & PTE_PHYS_MASK);
                uint8_t refCount = PmmGetRefCount(oldPhys);

                if (refCount > 1)
                {
                    // Shared COW page: allocate new page, copy, remap writable
                    PhysicalAddress newPhys = PmmAllocPage(MemTag::User, cowProc->pid);
                    if (newPhys)
                    {
                        auto* src = reinterpret_cast<const uint8_t*>(
                            PhysToVirt(oldPhys).raw());
                        auto* dst = reinterpret_cast<uint8_t*>(
                            PhysToVirt(newPhys).raw());
                        for (uint64_t b = 0; b < 4096; b += 8)
                            *reinterpret_cast<uint64_t*>(dst + b) =
                                *reinterpret_cast<const uint64_t*>(src + b);

                        // Update PTE: new phys, writable, clear COW bit
                        uint64_t newPte = (newPhys.raw() & PTE_PHYS_MASK)
                                        | ((*pte) & ~(PTE_PHYS_MASK | PTE_COW_BIT))
                                        | VMM_WRITABLE;
                        // Update PID in PTE to this process
                        newPte = (newPte & ~PTE_PID_MASK)
                               | (((uint64_t)cowProc->pid & 0x3FF) << PTE_PID_SHIFT);
                        *pte = newPte;

                        PmmUnrefPage(oldPhys);

                        __asm__ volatile("invlpg (%0)" :: "r"(cr2cow & ~0xFFFULL) : "memory");
                        __asm__ volatile("sti");
                        return;
                    }
                    // OOM — fall through to normal fault handling
                }
                else
                {
                    // Last reference: just make writable, clear COW
                    *pte = ((*pte) & ~PTE_COW_BIT) | VMM_WRITABLE;
                    __asm__ volatile("invlpg (%0)" :: "r"(cr2cow & ~0xFFFULL) : "memory");
                    __asm__ volatile("sti");
                    return;
                }
            }
        }

        // Safety net: present + write + user page without COW bit.
        // If the PTE belongs to this process, it likely lost its W bit
        // due to a memory visibility race — just set it writable.
        if (pfPresent && pfWrite && isUserAddr && cowProc)
        {
            using namespace brook;
            uint64_t* pte = VmmGetPte(cowProc->pageTable,
                                       VirtualAddress(cr2cow & ~0xFFFULL));
            if (pte && (*pte & VMM_PRESENT) && !(*pte & PTE_COW_BIT)
                && (*pte & VMM_USER))
            {
                uint16_t ptePid = static_cast<uint16_t>(
                    (*pte >> PTE_PID_SHIFT) & 0x3FF);
                if (ptePid == cowProc->pid)
                {
                    SerialPrintf("PF: recovering stale RO page at 0x%lx "
                                 "(pid %u, PTE=0x%lx)\n",
                                 cr2cow, cowProc->pid, *pte);
                    *pte |= VMM_WRITABLE;
                    __asm__ volatile("invlpg (%0)" :: "r"(cr2cow & ~0xFFFULL) : "memory");
                    __asm__ volatile("sti");
                    return;
                }
            }
        }
    }

    // For kernel-mode faults, delegate to the original handler for diagnostics + halt.
    if (!fromUser)
    {
        InterruptFrame ifrm;
        ifrm.ip    = ef->rip;
        ifrm.cs    = ef->cs;
        ifrm.flags = ef->rflags;
        ifrm.sp    = ef->rsp;
        ifrm.ss    = ef->ss;
        HandleException(static_cast<uint8_t>(vector), &ifrm, ef->errorCode, true);
        // HandleException never returns for kernel faults.
        for (;;) __asm__ volatile("cli; hlt");
    }

    // --- User-mode fault ---
    brook::Process* proc = brook::ProcessCurrent();
    int signum = ExceptionToSignal(static_cast<uint8_t>(vector));

    uint64_t cr2 = 0;
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));

    // Note: COW page faults are handled above (before the kernel/user split)
    // for both kernel-mode and user-mode writes to COW-protected user pages.

    // Check if the process has a handler registered for this signal.
    bool hasHandler = false;
    if (proc && proc->pid < brook::MAX_PROCESSES)
    {
        const brook::KernelSigaction& sa = brook::g_sigHandlers[proc->pid][signum - 1];
        if (sa.handler > 1) // Not SIG_DFL (0) or SIG_IGN (1)
            hasHandler = true;
    }

    if (hasHandler && proc && !proc->inSignalHandler)
    {
        const brook::KernelSigaction& sa = brook::g_sigHandlers[proc->pid][signum - 1];

        ExcPutsRaw("[SIG] Delivering signal ");
        ExcPutCharRaw(static_cast<char>('0' + signum / 10));
        ExcPutCharRaw(static_cast<char>('0' + signum % 10));
        ExcPutsRaw(" to pid ");
        {
            char pbuf[6];
            uint16_t pid = proc->pid;
            pbuf[0] = static_cast<char>('0' + (pid / 10000) % 10);
            pbuf[1] = static_cast<char>('0' + (pid / 1000) % 10);
            pbuf[2] = static_cast<char>('0' + (pid / 100) % 10);
            pbuf[3] = static_cast<char>('0' + (pid / 10) % 10);
            pbuf[4] = static_cast<char>('0' + pid % 10);
            pbuf[5] = '\0';
            const char* p = pbuf;
            while (*p == '0' && p[1] != '\0') ++p;
            ExcPutsRaw(p);
        }
        ExcPutsRaw(" (handler=0x");
        ExcPutHex(sa.handler);
        ExcPutsRaw(" faultAddr=0x");
        ExcPutHex(cr2);
        ExcPutsRaw(")\n");

        // Mark that we're in a signal handler
        proc->inSignalHandler = true;
        proc->sigSavedMask = proc->sigMask;
        proc->sigMask |= sa.mask | (1ULL << (signum - 1));
        proc->sigMask &= ~((1ULL << 8) | (1ULL << 18)); // never block SIGKILL/SIGSTOP

        // Build SignalFrame on the user stack
        uint64_t userRsp = ef->rsp;
        userRsp -= 128;                        // skip red zone
        userRsp -= sizeof(brook::SignalFrame);
        userRsp &= ~0xFULL;                   // 16-byte align

        auto* sf = reinterpret_cast<brook::SignalFrame*>(userRsp);

        // Clear the frame
        for (uint64_t i = 0; i < sizeof(brook::SignalFrame) / 8; i++)
            reinterpret_cast<uint64_t*>(sf)[i] = 0;

        // Return address: sa_restorer (musl's __restore_rt → rt_sigreturn)
        sf->pretcode = sa.restorer;

        // Fill ucontext
        sf->uc.uc_sigmask = proc->sigSavedMask;

        // Save all registers from the fault context into mcontext
        brook::SignalMcontext& mc = sf->uc.uc_mcontext;
        mc.r8     = ef->r8;
        mc.r9     = ef->r9;
        mc.r10    = ef->r10;
        mc.r11    = ef->r11;
        mc.r12    = ef->r12;
        mc.r13    = ef->r13;
        mc.r14    = ef->r14;
        mc.r15    = ef->r15;
        mc.rdi    = ef->rdi;
        mc.rsi    = ef->rsi;
        mc.rbp    = ef->rbp;
        mc.rbx    = ef->rbx;
        mc.rdx    = ef->rdx;
        mc.rax    = ef->rax;
        mc.rcx    = ef->rcx;
        mc.rsp    = ef->rsp;
        mc.rip    = ef->rip;
        mc.eflags = ef->rflags;
        mc.cs     = 0x23;          // user code segment
        mc.err    = ef->errorCode;
        mc.trapno = vector;
        mc.cr2    = cr2;

        // Fill siginfo
        sf->info.si_signo = signum;
        sf->info.si_errno = 0;
        if (vector == 14)
            sf->info.si_code = (ef->errorCode & 1) ? SEGV_ACCERR : SEGV_MAPERR;
        else
            sf->info.si_code = SI_KERNEL;
        // Store faulting address in si_addr (first 8 bytes of _data)
        *reinterpret_cast<uint64_t*>(&sf->info._data[0]) = cr2;

        // Redirect execution: modify the FullExceptionFrame so IRETQ returns
        // to the signal handler with the right arguments.
        ef->rip    = sa.handler;               // RIP → handler
        ef->rsp    = userRsp;                  // RSP → signal frame
        ef->rdi    = static_cast<uint64_t>(signum);        // arg1 = signum
        if (sa.flags & brook::SA_SIGINFO)
        {
            ef->rsi = reinterpret_cast<uint64_t>(&sf->info);  // arg2 = siginfo
            ef->rdx = reinterpret_cast<uint64_t>(&sf->uc);    // arg3 = ucontext
        }

        // Re-enable interrupts; the assembly stub will IRETQ back to the handler.
        __asm__ volatile("sti");
        return;
    }

    // No handler registered — print diagnostics and kill the process.
    InterruptFrame ifrm;
    ifrm.ip    = ef->rip;
    ifrm.cs    = ef->cs;
    ifrm.flags = ef->rflags;
    ifrm.sp    = ef->rsp;
    ifrm.ss    = ef->ss;
    HandleException(static_cast<uint8_t>(vector), &ifrm, ef->errorCode, true, true /*swapgsDone*/);
    // HandleException never returns for user-mode faults (it exits the process).
    for (;;) __asm__ volatile("cli; hlt");
}

// ---- Exception stubs: no error code ----
#define EXC_NOERR(N) \
    __attribute__((interrupt)) \
    static void ExceptionHandler##N(InterruptFrame* frame) { \
        HandleException(N, frame, 0, false); \
    }

// ---- Exception stubs: with error code ----
#define EXC_ERR(N) \
    __attribute__((interrupt)) \
    static void ExceptionHandler##N(InterruptFrame* frame, uintptr_t err) { \
        HandleException(N, frame, static_cast<uint64_t>(err), true); \
    }

EXC_NOERR(0)   // #DE Divide Error
EXC_NOERR(1)   // #DB Debug
EXC_NOERR(2)   // NMI
EXC_NOERR(3)   // #BP Breakpoint
EXC_NOERR(4)   // #OF Overflow
EXC_NOERR(5)   // #BR Bound Range
EXC_NOERR(6)   // #UD Invalid Opcode
EXC_NOERR(7)   // #NM Device Not Available
EXC_ERR(8)     // #DF Double Fault (error code always 0)
EXC_NOERR(9)   // Coprocessor Segment Overrun (legacy)
EXC_ERR(10)    // #TS Invalid TSS
EXC_ERR(11)    // #NP Segment Not Present
EXC_ERR(12)    // #SS Stack-Segment Fault
// Vectors 13 (#GP) and 14 (#PF) use assembly stubs from exception_stubs.S
// that provide full GPR access for signal delivery (SIGSEGV).
extern "C" void ExceptionStub13();
extern "C" void ExceptionStub14();
EXC_NOERR(15)  // Reserved
EXC_NOERR(16)  // #MF x87 FPU Exception
EXC_ERR(17)    // #AC Alignment Check
EXC_NOERR(18)  // #MC Machine Check
EXC_NOERR(19)  // #XM/#XF SIMD Exception
EXC_NOERR(20)  // #VE Virtualization Exception
EXC_ERR(21)    // #CP Control Protection
EXC_NOERR(22)  // Reserved
EXC_NOERR(23)  // Reserved
EXC_NOERR(24)  // Reserved
EXC_NOERR(25)  // Reserved
EXC_NOERR(26)  // Reserved
EXC_NOERR(27)  // Reserved
EXC_NOERR(28)  // #HV Hypervisor Injection
EXC_ERR(29)    // #VC VMM Communication
EXC_ERR(30)    // #SX Security Exception
EXC_NOERR(31)  // Reserved

// ---- Spurious IRQ stubs (vectors 32-47) ----
// The legacy 8259 PIC is disabled and fully masked during ApicInit().
// These vectors should never fire; if they do, panic rather than silently ignore.
#define IRQ_SPURIOUS(N) \
    __attribute__((interrupt)) \
    static void IrqHandler##N(InterruptFrame* frame) { \
        HandleException(N, frame, 0, false); \
    }

IRQ_SPURIOUS(32)
IRQ_SPURIOUS(33)
IRQ_SPURIOUS(34)
IRQ_SPURIOUS(35)
IRQ_SPURIOUS(36)
IRQ_SPURIOUS(37)
IRQ_SPURIOUS(38)
IRQ_SPURIOUS(39)
IRQ_SPURIOUS(40)
IRQ_SPURIOUS(41)
IRQ_SPURIOUS(42)
IRQ_SPURIOUS(43)
IRQ_SPURIOUS(44)
IRQ_SPURIOUS(45)
IRQ_SPURIOUS(46)
IRQ_SPURIOUS(47)

// ---------------------------------------------------------------------------
// Shared IRQ handler chain — supports multiple drivers on the same IOAPIC IRQ
// ---------------------------------------------------------------------------

// Handlers registered here must be plain functions (NOT __attribute__((interrupt))).
// They must NOT call ApicSendEoi() — the dispatch stub handles that.
using IrqHandlerFn = void (*)();

static constexpr int MAX_IRQ_CHAIN = 4;
static constexpr int MAX_SHARED_IRQS = 24;

struct SharedIrqEntry {
    uint8_t      vector;
    uint8_t      irq;
    uint8_t      count;
    IrqHandlerFn handlers[MAX_IRQ_CHAIN];
};

static SharedIrqEntry g_sharedIrqs[MAX_SHARED_IRQS];
static int g_sharedIrqCount = 0;

static SharedIrqEntry* FindSharedIrq(uint8_t irq)
{
    for (int i = 0; i < g_sharedIrqCount; i++)
        if (g_sharedIrqs[i].irq == irq) return &g_sharedIrqs[i];
    return nullptr;
}

// Per-IRQ dispatch stubs. Each is an __attribute__((interrupt)) function that
// iterates handlers[0..count) for one SharedIrqEntry, then sends EOI.
// We generate a small fixed set (one per MAX_SHARED_IRQS slot).
#define SHARED_IRQ_STUB(N) \
    __attribute__((interrupt)) \
    static void SharedIrqStub##N(InterruptFrame* frame) { \
        (void)frame; \
        SharedIrqEntry& e = g_sharedIrqs[N]; \
        for (int j = 0; j < e.count; j++) \
            e.handlers[j](); \
        ApicSendEoi(); \
    }

SHARED_IRQ_STUB(0)
SHARED_IRQ_STUB(1)
SHARED_IRQ_STUB(2)
SHARED_IRQ_STUB(3)

using StubFn = void (*)(InterruptFrame*);
static StubFn g_sharedIrqStubs[] = {
    SharedIrqStub0, SharedIrqStub1, SharedIrqStub2, SharedIrqStub3
};

// Register a plain (non-interrupt) handler for an IOAPIC IRQ.
// The handler must NOT be __attribute__((interrupt)) and must NOT call ApicSendEoi().
// IoApicRegisterHandler installs a proper interrupt stub that dispatches to all
// chained handlers and sends EOI once.
uint8_t IoApicRegisterHandler(uint8_t irq, uint8_t preferredVector, void* handler)
{
    SharedIrqEntry* existing = FindSharedIrq(irq);
    if (existing)
    {
        // IRQ already mapped — add to chain
        if (existing->count < MAX_IRQ_CHAIN)
        {
            existing->handlers[existing->count++] =
                reinterpret_cast<IrqHandlerFn>(handler);
            SerialPrintf("IRQ: chained handler on IRQ %u (vector %u, %u handlers)\n",
                         irq, existing->vector, existing->count);
            // Stub already installed on first registration — no IDT change needed
        }
        else
        {
            SerialPrintf("IRQ: WARNING — chain full for IRQ %u\n", irq);
        }
        return existing->vector;
    }

    // New IRQ — allocate a slot, install per-slot stub, unmask
    int slot = g_sharedIrqCount;
    if (slot < static_cast<int>(sizeof(g_sharedIrqStubs) / sizeof(g_sharedIrqStubs[0])))
    {
        SharedIrqEntry& e = g_sharedIrqs[g_sharedIrqCount++];
        e.irq = irq;
        e.vector = preferredVector;
        e.count = 1;
        e.handlers[0] = reinterpret_cast<IrqHandlerFn>(handler);

        // Always use the per-slot stub (even for single handler) so we
        // can add more handlers later without swapping the IDT entry.
        IdtInstallHandler(preferredVector,
                          reinterpret_cast<void*>(g_sharedIrqStubs[slot]));
        IoApicUnmaskIrq(irq, preferredVector);
        return preferredVector;
    }

    SerialPrintf("IRQ: WARNING — shared IRQ table full\n");
    return preferredVector;
}

void IdtInstallHandler(uint8_t vector, void* handler)
{
    SetIdtEntry(vector, handler);
    __asm__ volatile("lidt %0" : : "m"(g_idtDesc));
}

void IdtInit(brook::Framebuffer* fb)
{
    (void)fb; // KernelPanic handles the framebuffer via TtyReady()

    // Exception handlers 0-31
    SetIdtEntry( 0, reinterpret_cast<void*>(ExceptionHandler0));
    SetIdtEntry( 1, reinterpret_cast<void*>(ExceptionHandler1));
    SetIdtEntry( 2, reinterpret_cast<void*>(ExceptionHandler2));
    SetIdtEntry( 3, reinterpret_cast<void*>(ExceptionHandler3));
    SetIdtEntry( 4, reinterpret_cast<void*>(ExceptionHandler4));
    SetIdtEntry( 5, reinterpret_cast<void*>(ExceptionHandler5));
    SetIdtEntry( 6, reinterpret_cast<void*>(ExceptionHandler6));
    SetIdtEntry( 7, reinterpret_cast<void*>(ExceptionHandler7));
    // #DF double-fault MUST use IST1 so it fires even if the kernel stack is corrupt.
    SetIdtEntry( 8, reinterpret_cast<void*>(ExceptionHandler8),  IST_DOUBLE_FAULT);
    SetIdtEntry( 9, reinterpret_cast<void*>(ExceptionHandler9));
    SetIdtEntry(10, reinterpret_cast<void*>(ExceptionHandler10));
    SetIdtEntry(11, reinterpret_cast<void*>(ExceptionHandler11));
    // #SS, #GP, #PF use the normal kernel stack (ist=0) — it's safe now that
    // we have a dedicated 32KB stack.  Only #DF needs IST1.
    SetIdtEntry(12, reinterpret_cast<void*>(ExceptionHandler12));
    SetIdtEntry(13, reinterpret_cast<void*>(ExceptionStub13));  // asm stub for SIGSEGV delivery
    SetIdtEntry(14, reinterpret_cast<void*>(ExceptionStub14));  // asm stub for SIGSEGV delivery
    SetIdtEntry(15, reinterpret_cast<void*>(ExceptionHandler15));
    SetIdtEntry(16, reinterpret_cast<void*>(ExceptionHandler16));
    SetIdtEntry(17, reinterpret_cast<void*>(ExceptionHandler17));
    SetIdtEntry(18, reinterpret_cast<void*>(ExceptionHandler18));
    SetIdtEntry(19, reinterpret_cast<void*>(ExceptionHandler19));
    SetIdtEntry(20, reinterpret_cast<void*>(ExceptionHandler20));
    SetIdtEntry(21, reinterpret_cast<void*>(ExceptionHandler21));
    SetIdtEntry(22, reinterpret_cast<void*>(ExceptionHandler22));
    SetIdtEntry(23, reinterpret_cast<void*>(ExceptionHandler23));
    SetIdtEntry(24, reinterpret_cast<void*>(ExceptionHandler24));
    SetIdtEntry(25, reinterpret_cast<void*>(ExceptionHandler25));
    SetIdtEntry(26, reinterpret_cast<void*>(ExceptionHandler26));
    SetIdtEntry(27, reinterpret_cast<void*>(ExceptionHandler27));
    SetIdtEntry(28, reinterpret_cast<void*>(ExceptionHandler28));
    SetIdtEntry(29, reinterpret_cast<void*>(ExceptionHandler29));
    SetIdtEntry(30, reinterpret_cast<void*>(ExceptionHandler30));
    SetIdtEntry(31, reinterpret_cast<void*>(ExceptionHandler31));

    // Spurious IRQ stubs 32-47 (PIC disabled; these should never fire)
    SetIdtEntry(32, reinterpret_cast<void*>(IrqHandler32));
    SetIdtEntry(33, reinterpret_cast<void*>(IrqHandler33));
    SetIdtEntry(34, reinterpret_cast<void*>(IrqHandler34));
    SetIdtEntry(35, reinterpret_cast<void*>(IrqHandler35));
    SetIdtEntry(36, reinterpret_cast<void*>(IrqHandler36));
    SetIdtEntry(37, reinterpret_cast<void*>(IrqHandler37));
    SetIdtEntry(38, reinterpret_cast<void*>(IrqHandler38));
    SetIdtEntry(39, reinterpret_cast<void*>(IrqHandler39));
    SetIdtEntry(40, reinterpret_cast<void*>(IrqHandler40));
    SetIdtEntry(41, reinterpret_cast<void*>(IrqHandler41));
    SetIdtEntry(42, reinterpret_cast<void*>(IrqHandler42));
    SetIdtEntry(43, reinterpret_cast<void*>(IrqHandler43));
    SetIdtEntry(44, reinterpret_cast<void*>(IrqHandler44));
    SetIdtEntry(45, reinterpret_cast<void*>(IrqHandler45));
    SetIdtEntry(46, reinterpret_cast<void*>(IrqHandler46));
    SetIdtEntry(47, reinterpret_cast<void*>(IrqHandler47));

    g_idtDesc.limit = static_cast<uint16_t>(sizeof(g_idt) - 1);
    g_idtDesc.base  = reinterpret_cast<uint64_t>(&g_idt);

    __asm__ volatile("lidt %0" : : "m"(g_idtDesc));
}

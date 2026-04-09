#include "idt.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"

// ---- IDT storage ----
static IdtEntry      g_idt[256];
static IdtDescriptor g_idtDesc;

// ---- Helper to fill one IDT entry ----
static void SetIdtEntry(uint8_t vector, void* handler)
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(handler);
    g_idt[vector].offsetLow  = static_cast<uint16_t>(addr & 0xFFFF);
    g_idt[vector].selector   = GDT_KERNEL_CODE;
    g_idt[vector].ist        = 0;
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
static void HandleException(uint8_t vector, InterruptFrame* frame, uint64_t errorCode, bool hasErrorCode)
{
    // Read CR2 here (page fault address) — valid before any stack manipulation.
    uint64_t cr2 = 0;
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));

    const char* name = (vector < 32) ? g_excNames[vector] : "Unknown";

    if (vector == 14) // #PF — include fault address and error code breakdown
    {
        KernelPanic(
            "Exception %u: %s\n"
            "  Fault addr  0x%lx\n"
            "  Error code  0x%lx  [%s%s%s%s]\n"
            "  RIP  0x%lx   CS   0x%lx\n"
            "  RSP  0x%lx   SS   0x%lx\n"
            "  RFLAGS 0x%lx\n",
            static_cast<unsigned>(vector), name,
            cr2,
            errorCode,
            (errorCode & 1) ? "P " : "NP ",   // Present
            (errorCode & 2) ? "W " : "R ",    // Write/Read
            (errorCode & 4) ? "U " : "K ",    // User/Kernel
            (errorCode & 8) ? "RSVD " : "",   // Reserved write
            frame->ip, frame->cs,
            frame->sp, frame->ss,
            frame->flags
        );
    }
    else if (hasErrorCode)
    {
        KernelPanic(
            "Exception %u: %s\n"
            "  Error code  0x%lx\n"
            "  RIP  0x%lx   CS   0x%lx\n"
            "  RSP  0x%lx   SS   0x%lx\n"
            "  RFLAGS 0x%lx\n",
            static_cast<unsigned>(vector), name,
            errorCode,
            frame->ip, frame->cs,
            frame->sp, frame->ss,
            frame->flags
        );
    }
    else
    {
        KernelPanic(
            "Exception %u: %s\n"
            "  RIP  0x%lx   CS   0x%lx\n"
            "  RSP  0x%lx   SS   0x%lx\n"
            "  RFLAGS 0x%lx\n",
            static_cast<unsigned>(vector), name,
            frame->ip, frame->cs,
            frame->sp, frame->ss,
            frame->flags
        );
    }
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
EXC_ERR(13)    // #GP General Protection
EXC_ERR(14)    // #PF Page Fault
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
    SetIdtEntry( 8, reinterpret_cast<void*>(ExceptionHandler8));
    SetIdtEntry( 9, reinterpret_cast<void*>(ExceptionHandler9));
    SetIdtEntry(10, reinterpret_cast<void*>(ExceptionHandler10));
    SetIdtEntry(11, reinterpret_cast<void*>(ExceptionHandler11));
    SetIdtEntry(12, reinterpret_cast<void*>(ExceptionHandler12));
    SetIdtEntry(13, reinterpret_cast<void*>(ExceptionHandler13));
    SetIdtEntry(14, reinterpret_cast<void*>(ExceptionHandler14));
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

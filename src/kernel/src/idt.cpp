#include "idt.h"
#include "gdt.h"
#include "serial.h"

// ---- Port I/O helpers ----
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// ---- Panic state ----
static brook::Framebuffer* g_panicFb = nullptr;

// ---- IDT storage ----
static IdtEntry     g_idt[48];
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

// ---- Common exception handler ----
static void HandleException(uint8_t vector, InterruptFrame* frame, uint64_t errorCode)
{
    (void)errorCode;
    brook::SerialPrintf("EXCEPTION %u at RIP=0x%p\n",
                        static_cast<unsigned>(vector),
                        reinterpret_cast<void*>(frame->ip));

    if (g_panicFb) {
        uint32_t* pixels = reinterpret_cast<uint32_t*>(g_panicFb->physicalBase);
        uint32_t  stride = g_panicFb->stride / 4;
        for (uint32_t y = 0; y < g_panicFb->height; y++) {
            for (uint32_t x = 0; x < g_panicFb->width; x++) {
                pixels[y * stride + x] = 0x00FF0000u;  // red
            }
        }
    }

    __asm__ volatile("cli");
    for (;;) { __asm__ volatile("hlt"); }
}

// ---- Exception stubs: no error code ----
#define EXC_NOERR(N) \
    __attribute__((interrupt)) \
    static void ExceptionHandler##N(InterruptFrame* frame) { \
        HandleException(N, frame, 0); \
    }

// ---- Exception stubs: with error code ----
#define EXC_ERR(N) \
    __attribute__((interrupt)) \
    static void ExceptionHandler##N(InterruptFrame* frame, uintptr_t err) { \
        HandleException(N, frame, static_cast<uint64_t>(err)); \
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

// ---- IRQ stubs (vectors 32-47) ----
// Master PIC IRQs 0-7 → vectors 32-39: EOI to master only
#define IRQ_MASTER(N) \
    __attribute__((interrupt)) \
    static void IrqHandler##N(InterruptFrame* frame) { \
        (void)frame; \
        outb(0x20, 0x20); \
    }

// Slave PIC IRQs 8-15 → vectors 40-47: EOI to slave then master
#define IRQ_SLAVE(N) \
    __attribute__((interrupt)) \
    static void IrqHandler##N(InterruptFrame* frame) { \
        (void)frame; \
        outb(0xA0, 0x20); \
        outb(0x20, 0x20); \
    }

IRQ_MASTER(32)
IRQ_MASTER(33)
IRQ_MASTER(34)
IRQ_MASTER(35)
IRQ_MASTER(36)
IRQ_MASTER(37)
IRQ_MASTER(38)
IRQ_MASTER(39)
IRQ_SLAVE(40)
IRQ_SLAVE(41)
IRQ_SLAVE(42)
IRQ_SLAVE(43)
IRQ_SLAVE(44)
IRQ_SLAVE(45)
IRQ_SLAVE(46)
IRQ_SLAVE(47)

void IdtInit(brook::Framebuffer* fb)
{
    g_panicFb = fb;

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

    // IRQ stubs 32-47
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

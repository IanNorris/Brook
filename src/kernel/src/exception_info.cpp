// exception_info.cpp — human-readable exception descriptions for panic screens.
// Compiled with -mgeneral-regs-only (safe for interrupt context).

#include "exception_info.h"

namespace brook {

static const char* g_excNames[] = {
    "#DE Divide Error",                    // 0
    "#DB Debug",                           // 1
    "NMI Non-Maskable Interrupt",          // 2
    "#BP Breakpoint",                      // 3
    "#OF Overflow",                        // 4
    "#BR Bound Range Exceeded",            // 5
    "#UD Invalid Opcode",                  // 6
    "#NM Device Not Available",            // 7
    "#DF Double Fault",                    // 8
    "Coprocessor Segment Overrun",         // 9
    "#TS Invalid TSS",                     // 10
    "#NP Segment Not Present",             // 11
    "#SS Stack-Segment Fault",             // 12
    "#GP General Protection Fault",        // 13
    "#PF Page Fault",                      // 14
    "(Reserved)",                          // 15
    "#MF x87 FPU Error",                   // 16
    "#AC Alignment Check",                 // 17
    "#MC Machine Check",                   // 18
    "#XM SIMD Exception",                  // 19
    "#VE Virtualization Exception",        // 20
    "#CP Control Protection Exception",    // 21
};

static constexpr uint32_t NUM_EXC_NAMES = sizeof(g_excNames) / sizeof(g_excNames[0]);

const char* ExceptionName(uint8_t vector)
{
    if (vector < NUM_EXC_NAMES)
        return g_excNames[vector];
    return "Unknown Exception";
}

const char* ExceptionDescribe(uint8_t vector, uint64_t errorCode, uint64_t cr2,
                               uint64_t rip, bool fromUser)
{
    static constexpr uint64_t KERNEL_BASE = 0xffffffff80000000ULL;

    switch (vector)
    {
    case 0:  return "CPU attempted integer division by zero.";
    case 6:  return "CPU encountered an unrecognised instruction.";
    case 8:  return "A second exception occurred while handling the first (double fault).";
    case 12: return "Stack segment fault — possible stack overflow or corrupt stack pointer.";
    case 13: // #GP
        if (errorCode != 0)
            return "General protection fault with non-zero selector error code.";
        if (rip >= KERNEL_BASE)
            return "Kernel accessed an invalid or non-canonical memory address.";
        return "Process performed an invalid memory access or privileged operation.";

    case 14: // #PF
    {
        bool present   = (errorCode & 1) != 0;
        bool write     = (errorCode & 2) != 0;
        bool user      = (errorCode & 4) != 0;
        bool exec      = (errorCode & 16) != 0;

        if (exec && !present)
            return "Attempted to execute code from an unmapped page.";
        if (exec && present)
            return "Attempted to execute code on a non-executable page.";
        if (cr2 < 0x1000)
            return write ? "Null pointer dereference (write)."
                         : "Null pointer dereference (read).";
        if (!present && write)
            return "Write to an unmapped virtual address.";
        if (!present && !write)
            return "Read from an unmapped virtual address.";
        if (present && write && !user)
            return "Kernel write to a read-only page (possible COW or guard page).";
        if (present && write)
            return "Write to a read-only page.";
        return "Page fault — access violation.";
    }

    case 18: return "Machine check exception — hardware error detected.";
    default: return "An unexpected processor exception occurred.";
    }
}

} // namespace brook

#pragma once

// KernelPanic — called on unrecoverable errors.
//
// - Disables interrupts immediately.
// - Prints a red-on-black "KERNEL PANIC" banner to TTY (if ready) and serial.
// - Dumps RIP, RSP, CR2, CR3.
// - Halts forever.
//
// panic.cpp is compiled with -mgeneral-regs-only so it is safe to call from
// interrupt context (no XMM state touched).
//
// KERNEL_ASSERT(cond, msg) panics with file/line info if cond is false.

[[noreturn]] void KernelPanic(const char* fmt, ...);

#define KERNEL_ASSERT(cond, msg) \
    do { \
        if (__builtin_expect(!(cond), 0)) { \
            KernelPanic("ASSERT failed: " msg "\n  at %s:%d (%s)\n", \
                        __FILE__, __LINE__, __func__); \
        } \
    } while (0)

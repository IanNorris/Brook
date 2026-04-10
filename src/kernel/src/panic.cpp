// panic.cpp — compiled with -mgeneral-regs-only so it is safe to call from
// interrupt context.  No SSE/FPU registers are touched here.
#include "panic.h"
#include "serial.h"
#include "tty.h"
#include "panic_qr.h"

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

static void TtyPutHex64(uint64_t v)
{
    if (!brook::TtyReady()) return;
    brook::TtyPuts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        int nib = static_cast<int>((v >> shift) & 0xF);
        brook::TtyPutChar(static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10));
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

    // Format the message into a static buffer.
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    PanicFormatStr(g_panicBuf, static_cast<int>(sizeof(g_panicBuf)), fmt, args);
    __builtin_va_end(args);

    // -- Serial output (always available) ------------------------------------
    brook::SerialPuts("\n*** KERNEL PANIC ***\n");
    brook::SerialPuts(g_panicBuf);
    brook::SerialPuts("RIP "); SerialPutHex64(regs.rip);
    brook::SerialPuts("  RSP "); SerialPutHex64(regs.rsp);
    brook::SerialPuts("\nCR2 "); SerialPutHex64(regs.cr2);
    brook::SerialPuts("  CR3 "); SerialPutHex64(regs.cr3);
    brook::SerialPuts("\nSystem halted.\n");

    // -- TTY output (if framebuffer is up) ------------------------------------
    if (brook::TtyReady())
    {
        brook::TtySetColors(0xFFFFFF, 0xCC0000); // white on red
        brook::TtyPuts("\n*** KERNEL PANIC ***\n");
        brook::TtySetColors(0xFFFFFF, 0x1A0000); // white on dark red
        brook::TtyPuts(g_panicBuf);
        brook::TtyPuts("RIP "); TtyPutHex64(regs.rip);
        brook::TtyPuts("  RSP "); TtyPutHex64(regs.rsp);
        brook::TtyPuts("\nCR2 "); TtyPutHex64(regs.cr2);
        brook::TtyPuts("  CR3 "); TtyPutHex64(regs.cr3);
        brook::TtyPuts("\nSystem halted.\n");
    }

    // -- QR code (if framebuffer is available) ---------------------------------
    uint32_t* fbPixels = nullptr;
    uint32_t fbW = 0, fbH = 0, fbStride = 0;
    if (brook::TtyGetFramebuffer(&fbPixels, &fbW, &fbH, &fbStride))
    {
        brook::PanicRenderQR(fbPixels, fbW, fbH, fbStride, &fullRegs);
    }

    for (;;) { __asm__ volatile("hlt"); }
}

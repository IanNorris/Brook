#include "serial.h"

// Standalone test kernels do not link the production panic UI. A PMM invariant
// violation must still report failure and terminate QEMU, rather than omit the
// check or leave the test looking hung.
extern "C" __attribute__((noreturn)) void KernelPanic(const char* format, ...) {
    brook::SerialPrintf("[FAIL] invariant panic: %s\n", format);
    const uint8_t failure=1;
    __asm__ volatile("outb %0, $0xf4"::"a"(failure));
    for (;;) __asm__ volatile("cli; hlt");
}

#pragma once
// panic_screen.h — Full-screen Enkel-style panic display.
//
// Renders a formatted crash screen directly to the framebuffer with:
//   - Dark red background
//   - "KERNEL PANIC" banner
//   - Build info, error description
//   - Register dump (left column)
//   - Symbolicated stack trace (left column)
//   - QR code (right column)
//
// No TTY dependency — renders glyphs directly from font atlas.

#include <stdint.h>
#include "panic_qr.h"

namespace brook {

struct PanicScreenInfo {
    const char*              message;      // panic message text
    const PanicCPURegs*      regs;         // CPU registers at crash
    const PanicStackTrace*   trace;        // stack trace
    const PanicExceptionInfo* excInfo;     // exception details (nullptr for KernelPanic)
    const PanicProcessList*  procList;     // running processes snapshot
    uint64_t                 vector;       // exception vector (or 0 for KernelPanic)
    uint64_t                 errorCode;    // error code
};

// Render the full panic screen to the given framebuffer.
void PanicScreenRender(uint32_t* fb, uint32_t fbW, uint32_t fbH,
                       uint32_t fbStride, const PanicScreenInfo* info);

} // namespace brook

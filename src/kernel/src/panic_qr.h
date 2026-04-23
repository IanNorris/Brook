#pragma once
// QR code panic screen — encode CPU state into a scannable QR code.
//
// Data pipeline: CPURegs → binary packet → Base45 → Nayuki QR → framebuffer
// (zlib compression will be added later for smaller QR codes)
//
// Constants hand-tuned on real hardware (Surface Go) via Enkel project:
//   PixelsPerModule = 3
//   Position = (50, 50)
//   Quiet zone = 2 modules
//   Contrast = 2 for panic (slightly grey white for CRT/LCD readability)

#include <stdint.h>

namespace brook {

// Panic QR protocol constants
static constexpr uint8_t  QR_MAGIC_BYTE  = 0x2D;
static constexpr uint8_t  QR_VERSION     = 0x01;
static constexpr uint32_t QR_HEADER_PAD  = 0xCAFEF00D;
static constexpr uint32_t QR_PACKET_TYPE_CPU_REGS       = 0xA3000001;
static constexpr uint32_t QR_PACKET_TYPE_STACK_TRACE    = 0xA3000002;
static constexpr uint32_t QR_PACKET_TYPE_EXCEPTION_INFO = 0xA3000003;
static constexpr uint32_t QR_PACKET_TYPE_PROCESS_LIST   = 0xA3000004;

// Rendering constants (tuned on real hardware)
static constexpr uint32_t QR_PIXELS_PER_MODULE = 3;
static constexpr uint32_t QR_START_X           = 50;
static constexpr uint32_t QR_START_Y           = 50;
static constexpr int      QR_BORDER_WIDTH      = 2;
static constexpr int      QR_CONTRAST          = 2;

// Packet header
struct __attribute__((packed)) PanicHeader {
    uint8_t  magic;      // QR_MAGIC_BYTE
    uint8_t  version;    // QR_VERSION
    uint8_t  page;       // Current page (0-based)
    uint8_t  pageCount;  // Total pages
    uint32_t pad;        // QR_HEADER_PAD
};

struct __attribute__((packed)) PanicPacketHeader {
    uint32_t type;       // QR_PACKET_TYPE_CPU_REGS
    uint32_t size;       // sizeof(PanicCPURegs)
};

// CPU register state captured at panic time
struct __attribute__((packed)) PanicCPURegs {
    // General-purpose registers
    uint64_t rax, rbx, rcx, rdx, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    // Instruction pointer and stack
    uint64_t rip, rsp, rbp, rflags;

    // Control registers
    uint64_t cr0, cr2, cr3, cr4;

    // Segment registers
    uint16_t cs, ds, ss, es, fs, gs;

    // Padding to align
    uint16_t reserved;
};

// Stack trace captured at panic time (RBP frame walking)
static constexpr uint32_t PANIC_MAX_STACK_DEPTH = 16;

struct __attribute__((packed)) PanicStackTrace {
    uint8_t  depth;                              // number of valid frames
    uint64_t rip[PANIC_MAX_STACK_DEPTH];         // [0]=leaf, [1..]=callers
};

// Exception info captured at panic time
struct __attribute__((packed)) PanicExceptionInfo {
    uint8_t  vector;       // Exception vector number (0-31)
    uint8_t  reserved;
    uint16_t pid;          // PID of faulting process
    uint32_t errorCode;    // CPU error code
};

// Per-process summary for crash dump (compact: 20 bytes each)
static constexpr uint32_t PANIC_MAX_PROCESSES = 16;
static constexpr uint32_t PANIC_PROCESS_NAME_LEN = 12;

struct __attribute__((packed)) PanicProcessEntry {
    uint16_t pid;
    uint8_t  state;        // ProcessState enum
    uint8_t  cpu;          // runningOnCpu (0xFF = not running)
    char     name[PANIC_PROCESS_NAME_LEN];
    uint64_t rip;          // Last known RIP (0 if unavailable)
};

struct __attribute__((packed)) PanicProcessList {
    uint8_t count;
    PanicProcessEntry entries[PANIC_MAX_PROCESSES];
};

// Render a QR code containing CPU state + stack trace to the framebuffer.
// Called from KernelPanic after capturing registers.
// fbBase: physical address of framebuffer
// fbWidth/fbHeight: framebuffer dimensions
// fbStride: bytes per scanline
void PanicRenderQR(uint32_t* fbBase, uint32_t fbWidth, uint32_t fbHeight,
                   uint32_t fbStride, const PanicCPURegs* regs,
                   const PanicStackTrace* trace,
                   const PanicExceptionInfo* excInfo = nullptr,
                   const PanicProcessList* procList = nullptr);

} // namespace brook

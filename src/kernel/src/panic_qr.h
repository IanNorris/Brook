#pragma once
// QR code panic screen — encode CPU state into a scannable QR code.
//
// Data pipeline: CPURegs → binary TLV packet → Base45 → QR alphanumeric → framebuffer
// Uses Base45 encoding so QR codes are readable by any phone camera app.
// Multi-page support: large payloads are split across up to QR_MAX_PAGES QR codes.
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
static constexpr uint8_t  QR_VERSION     = 0x02;  // v2: LZ4 + Base45
static constexpr uint8_t  QR_VERSION_RAW = 0x01;  // v1: uncompressed binary

// Ingest URL rendered as a small SEPARATE static QR alongside the payload QR(s).
// A phone scans this first to open the Brook panic scanner site, which then reads
// the dense payload QR(s).  Kept as its own QR (not prefixed onto the payload) so
// the payload stays in QR alphanumeric mode (Base45) at full density — embedding
// a lowercase URL would force the whole code into byte mode and cut capacity ~58%.
#define PANIC_INGEST_URL "https://khione:9001/"
static constexpr uint32_t QR_HEADER_PAD  = 0xCAFEF00D;
static constexpr uint32_t QR_PACKET_TYPE_CPU_REGS       = 0xA3000001;
static constexpr uint32_t QR_PACKET_TYPE_STACK_TRACE    = 0xA3000002;
static constexpr uint32_t QR_PACKET_TYPE_EXCEPTION_INFO = 0xA3000003;
static constexpr uint32_t QR_PACKET_TYPE_PROCESS_LIST   = 0xA3000004;
static constexpr uint32_t QR_PACKET_TYPE_SYSTEM_INFO    = 0xA3000005;
static constexpr uint32_t QR_PACKET_TYPE_STACK_DUMP     = 0xA3000006;
// Extension packets — the TLV format lets us append optional, self-describing
// data sets. Decoders skip unknown types (advance by the packet's size field),
// so new extensions are backward/forward compatible by construction.
static constexpr uint32_t QR_PACKET_TYPE_PROCESS_EXT    = 0xA3000007; // BRO-176 reap gates
static constexpr uint32_t QR_PACKET_TYPE_CPU_STATE      = 0xA3000008; // per-CPU RIP/CR3/pid

// Rendering constants (tuned on real hardware — Enkel required dozens of iterations)
//
// QR_PIXELS_PER_MODULE: 3 works well on high-DPI displays.
//   Use 9 for low-DPI devices like Surface Go where the camera can't resolve
//   small modules.  The kernel auto-selects based on framebuffer resolution.
//
// QR_CONTRAST: reduces white from 0xFFFFFF to avoid camera bloom on screens.
//   2 → white = 0xDDDDDD (slightly off-white, good for most screens)
//   Range 0–7, higher = dimmer white.
//
// QR_INVERT_MODULES: when true, draws white modules on black background.
//   Can improve readability on some displays (especially OLED).
static constexpr uint32_t QR_PIXELS_PER_MODULE_HIDPI = 3;
static constexpr uint32_t QR_PIXELS_PER_MODULE_LODPI = 9;
static constexpr uint32_t QR_LODPI_THRESHOLD         = 1280; // fb width <= this uses LODPI
static constexpr uint32_t QR_START_X                 = 50;
static constexpr uint32_t QR_START_Y                 = 50;
static constexpr int      QR_BORDER_WIDTH            = 2;
static constexpr int      QR_CONTRAST                = 2;
static constexpr bool     QR_INVERT_MODULES          = false;
static constexpr uint8_t  QR_MAX_PAGES               = 4;

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

// Per-process summary for crash dump.  WIRE LAYOUT of the PROCESS_LIST packet is
// the original, stable 24 bytes (pid/state/cpu/name/rip) — do NOT change it.
// BRO-176 reap-gate data travels in a SEPARATE optional PROCESS_EXT packet (see
// PanicProcessExt), so old decoders ignore it and the core packet never moves.
// The two extra in-memory fields below are convenience storage filled by the
// capture sites; BuildPanicPayload serialises them into the ext packet.
static constexpr uint32_t PANIC_MAX_PROCESSES = 24;
static constexpr uint32_t PANIC_PROCESS_NAME_LEN = 12;
static constexpr uint32_t PANIC_PROCESS_ENTRY_WIRE_SIZE = 24; // pid+state+cpu+name+rip

// flags bits for PanicProcessExt::flags
static constexpr uint8_t PANIC_PROC_IS_THREAD   = 0x01;
static constexpr uint8_t PANIC_PROC_REAPABLE    = 0x02;
static constexpr uint8_t PANIC_PROC_IS_KTHREAD  = 0x04;
static constexpr uint8_t PANIC_PROC_MAGIC_BAD   = 0x08;

struct __attribute__((packed)) PanicProcessEntry {
    uint16_t pid;
    uint8_t  state;        // ProcessState enum
    uint8_t  cpu;          // runningOnCpu (0xFF = not running)
    char     name[PANIC_PROCESS_NAME_LEN];
    uint64_t rip;          // Last known RIP (0 if unavailable)
    // --- below here is NOT part of the PROCESS_LIST wire layout (ext packet) ---
    uint16_t tgid;         // thread-group id (leader pid)
    int16_t  asLiveThreads;// leader's live-thread count (-1 if not a leader)
    int16_t  refCount;     // process refcount
    uint8_t  flags;        // PANIC_PROC_* bits
};

// Wire layout of one PROCESS_EXT entry (keyed by pid so the decoder can merge it
// onto the PROCESS_LIST regardless of ordering): 10 bytes.
struct __attribute__((packed)) PanicProcessExtEntry {
    uint16_t pid;
    uint16_t tgid;
    int16_t  asLiveThreads;
    int16_t  refCount;
    uint8_t  flags;
    uint8_t  reserved;
};

struct __attribute__((packed)) PanicProcessList {
    uint8_t count;
    PanicProcessEntry entries[PANIC_MAX_PROCESSES];
};

// Per-CPU state at panic time — the single most useful addition for diagnosing
// hangs/deadlocks (a QR previously carried only the panicking CPU). The RIP is
// the live spin point if the panic NMI handler captured it (PANIC_CPU_LIVE_RIP
// set), otherwise the last-scheduled RIP. CR3 identifies which address space the
// CPU is in; pid names the process it last ran.
static constexpr uint32_t PANIC_MAX_CPUS_DUMP = 16;

// flags bits for PanicCpuEntry::flags
static constexpr uint8_t PANIC_CPU_ONLINE    = 0x01;
static constexpr uint8_t PANIC_CPU_HALTED    = 0x02;
static constexpr uint8_t PANIC_CPU_BSP       = 0x04;
static constexpr uint8_t PANIC_CPU_LIVE_RIP  = 0x08; // rip is the NMI-captured spin point

struct __attribute__((packed)) PanicCpuEntry {
    uint8_t  cpuIndex;
    uint8_t  flags;        // PANIC_CPU_* bits
    uint16_t pid;          // process last running on this CPU (0 if none)
    uint64_t rip;          // live spin RIP (if LIVE_RIP) or last-scheduled RIP
    uint64_t cr3;          // current address space root
};

struct __attribute__((packed)) PanicCpuList {
    uint8_t count;
    PanicCpuEntry entries[PANIC_MAX_CPUS_DUMP];
};

// System-level metadata for crash diagnosis
static constexpr uint32_t PANIC_GIT_HASH_LEN   = 20;  // short git hash + dirty tree tag (null-terminated)
static constexpr uint32_t PANIC_GIT_BRANCH_LEN = 24;  // branch name (null-terminated, truncated)

struct __attribute__((packed)) PanicSystemInfo {
    uint8_t  cpuIndex;                        // CPU that panicked
    uint8_t  cpuCount;                        // total CPUs online
    uint16_t reserved;
    uint64_t tscTicks;                        // RDTSC at panic time
    uint64_t tssRsp0;                         // TSS RSP0 of faulting CPU
    char     gitHash[PANIC_GIT_HASH_LEN];     // short commit hash
    char     gitBranch[PANIC_GIT_BRANCH_LEN]; // branch name at build time
};

// Raw stack dump from RSP at panic time
static constexpr uint32_t PANIC_STACK_DUMP_BYTES = 256;

struct __attribute__((packed)) PanicStackDump {
    uint64_t rsp;                           // RSP value at capture time
    uint16_t length;                        // actual bytes captured (≤ PANIC_STACK_DUMP_BYTES)
    uint8_t  data[PANIC_STACK_DUMP_BYTES];  // raw memory from [RSP, RSP+length)
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
                   const PanicProcessList* procList = nullptr,
                   const PanicSystemInfo* sysInfo = nullptr,
                   const PanicStackDump* stackDump = nullptr,
                   const PanicCpuList* cpuList = nullptr);

// Fill a PanicCpuList from current kernel state (per-CPU process + CR3, and the
// NMI-captured spin RIP if available). Safe to call from the panicking CPU after
// the APs are halted. Returns the number of entries filled.
uint32_t PanicCaptureCpuList(PanicCpuList* out);

} // namespace brook

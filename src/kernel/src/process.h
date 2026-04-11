#pragma once

#include <stdint.h>
#include "memory/address.h"

namespace brook {

// Maximum number of open file descriptors per process.
static constexpr uint32_t MAX_FDS = 64;

// Maximum concurrent processes.
static constexpr uint32_t MAX_PROCESSES = 16;

// Program break limit (max heap size per process).
static constexpr uint64_t PROGRAM_BREAK_SIZE = 64 * 1024 * 1024; // 64 MB

// Default user stack size.
static constexpr uint64_t USER_STACK_SIZE = 128 * 1024; // 128 KB

// Per-process kernel stack size (2 pages = 8 KB).
static constexpr uint64_t KERNEL_STACK_SIZE = 8 * 1024;
static constexpr uint64_t KERNEL_STACK_PAGES = KERNEL_STACK_SIZE / 4096;

// Scheduler time slice in milliseconds (~10ms).
static constexpr uint64_t SCHED_TIMESLICE_MS = 10;

// User-space virtual address where ELF binaries are loaded.
// This is in the lower canonical half (user space).
static constexpr uint64_t USER_LOAD_BASE = 0x400000;

// User mmap region starts at 256MB (above ELF load area, below stack).
static constexpr uint64_t USER_MMAP_BASE = 0x10000000ULL;   // 256 MB
static constexpr uint64_t USER_MMAP_END  = 0x700000000000ULL; // well below stack

// User stack top (grows down from here).
static constexpr uint64_t USER_STACK_TOP = 0x7FFFFFFFE000ULL;

// ---------------------------------------------------------------------------
// Process scheduling state
// ---------------------------------------------------------------------------

enum class ProcessState : uint8_t
{
    Ready,          // In the run queue, eligible for scheduling
    Running,        // Currently executing on CPU
    Blocked,        // Waiting (sleep, I/O, mutex) — not in run queue
    Terminated,     // Finished, awaiting cleanup
};

// ---------------------------------------------------------------------------
// Saved CPU context — stored on context switch
// ---------------------------------------------------------------------------
// Layout must match the context_switch asm (push/pop order).

struct SavedContext
{
    // General-purpose registers
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    // RIP is implicitly saved as the return address on the kernel stack
    // RSP is the stack pointer when we switch away
    uint64_t rsp;       // Kernel stack pointer at switch point
    uint64_t rip;       // Resume address (set for first switch, then implicit)
    uint64_t rflags;

    // Segment bases
    uint64_t cr3;       // Page table physical address
    uint64_t fsBase;    // FS segment base (TLS)
};

// FXSAVE area must be 16-byte aligned, 512 bytes.
struct alignas(16) FxsaveArea
{
    uint8_t data[512];
};

// ---------------------------------------------------------------------------
// File descriptor entry
// ---------------------------------------------------------------------------

enum class FdType : uint8_t
{
    None = 0,
    Vnode,         // Regular VFS file
    DevFramebuf,   // /dev/fb0
    DevKeyboard,   // /dev/keyboard
    Pipe,          // Future
};

struct FdEntry
{
    FdType   type;
    uint8_t  flags;        // O_NONBLOCK, etc.
    uint16_t _pad;
    uint32_t refCount;
    void*    handle;       // VFS Vnode* or device-specific state
    uint64_t seekPos;      // Current file offset (for lseek)
};

// ---------------------------------------------------------------------------
// ELF binary descriptor (populated by the ELF loader)
// ---------------------------------------------------------------------------

struct ElfBinary
{
    uint64_t baseAddress;       // Virtual base where binary was loaded
    uint64_t allocatedSize;     // Total bytes mapped
    uint64_t entryPoint;        // e_entry
    uint64_t programBreakLow;   // End of loaded segments (heap starts here)
    uint64_t programBreakHigh;  // programBreakLow + PROGRAM_BREAK_SIZE

    // Program headers (needed for AT_PHDR auxv)
    uint64_t phdrVaddr;         // Virtual address of program headers in memory
    uint16_t phdrNum;           // Number of program headers
    uint16_t phdrEntSize;       // Size of each program header entry

    // TLS template
    uint8_t* tlsInitData;       // Initial TLS data (PT_TLS p_vaddr in loaded image)
    uint64_t tlsInitSize;       // Size of initialized TLS data (p_filesz)
    uint64_t tlsTotalSize;      // Total TLS size including BSS (p_memsz)
    uint16_t tlsAlign;          // TLS alignment
};

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

struct Process
{
    uint16_t pid;
    ProcessState state;
    uint8_t  _pad0;
    uint32_t _pad1;

    // Scheduler linked-list pointers (circular doubly-linked ready queue)
    Process* schedNext;
    Process* schedPrev;

    // Blocking info (e.g. nanosleep wakeup tick)
    uint64_t wakeupTick;        // g_lapicTickCount value to unblock at (0 = N/A)

    // Saved CPU context (written by context_switch asm)
    SavedContext savedCtx;
    FxsaveArea   fxsave;

    // Per-process kernel stack (for ring 3→0 transitions and syscalls)
    uint64_t kernelStackBase;   // Bottom of kernel stack allocation
    uint64_t kernelStackTop;    // Top of kernel stack (initial RSP0)

    // Per-process page table
    PageTable pageTable;

    // ELF binary info
    ElfBinary elf;

    // Memory
    uint64_t programBreak;      // Current brk() position
    uint64_t mmapNext;          // Next free user-space mmap virtual address

    // User stack
    uint64_t stackBase;         // Bottom of stack allocation (lowest address)
    uint64_t stackTop;          // Top of stack (highest usable address)

    // TLS
    uint64_t fsBase;            // FS segment base (TLS)

    // File descriptors
    FdEntry  fds[MAX_FDS];

    // Process name (for debug output)
    char name[32];
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Get the current process (for syscall handlers).
Process* ProcessCurrent();

// Create a new process, loading an ELF binary from a memory buffer.
// Returns null on failure.
Process* ProcessCreate(const uint8_t* elfData, uint64_t elfSize,
                       int argc, const char* const* argv,
                       int envc, const char* const* envp);

// Destroy a process and free all its resources.
void ProcessDestroy(Process* proc);

// File descriptor operations
int       FdAlloc(Process* proc, FdType type, void* handle);
void      FdFree(Process* proc, int fd);
FdEntry*  FdGet(Process* proc, int fd);

} // namespace brook

// crash_dump.h — Brook user-mode crash-dump ABI.
//
// CrashCtx is the POD snapshot the kernel hands to the user-mode writer
// thread when a synchronous fatal exception hits a process that has
// registered a crash entry (syscall 502).  The writer runs inside the
// crashed process (shared VAS / fds) but on a fresh thread stack, so
// the only state it can trust is whatever is in *this* struct.
//
// Stable ABI: do not reorder; add new fields at the end only.

#ifndef BROOK_CRASH_DUMP_H
#define BROOK_CRASH_DUMP_H

#include <stdint.h>

#define BROOK_CRASH_CTX_MAGIC  0x43524143u  // 'CRAC'
#define BROOK_CRASH_CTX_VER    1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BrookCrashCtx {
    uint32_t magic;       // BROOK_CRASH_CTX_MAGIC
    uint32_t version;     // BROOK_CRASH_CTX_VER
    uint32_t signum;      // POSIX signal number (SIGSEGV=11, SIGILL=4, ...)
    uint32_t vector;      // CPU exception vector (14=#PF, 13=#GP, ...)
    uint64_t errorCode;   // CPU error code (pushed by hardware)
    uint64_t faultAddr;   // CR2 for #PF, zero otherwise
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs;
    uint64_t ss;
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t fsBase, gsBase;
    uint64_t tid;         // Thread that took the fault
    uint64_t timestampNs; // kernel monotonic timestamp
    char     commName[32]; // process name (NUL-padded)
} BrookCrashCtx;

// Entry point the kernel will invoke (on a new thread in the crashed
// process).  Provided by libcrashdump.  Never returns.
void __brook_crash_entry(const BrookCrashCtx* ctx);

#ifdef __cplusplus
}
#endif

#endif

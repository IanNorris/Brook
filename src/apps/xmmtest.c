// xmmtest.c — BRO-187 root-cause test: does the Brook kernel preserve userspace
// SSE/XMM registers across kernel entries (syscalls + timer interrupts)?
//
// The kernel is built with -msse2 globally (only a few files are
// -mgeneral-regs-only), so kernel C code uses XMM. If kernel entry/exit does
// not save/restore the user FPU/SSE state, a syscall or timer interrupt that
// returns to the same thread will clobber the user's XMM registers. libLLVM's
// global constructors keep live pointer data in XMM across such windows, so
// this would corrupt them (observed: a movaps stored {1,1} instead of valid
// pointers).
//
// Loads distinct sentinels into xmm0..xmm15, then (a) does syscalls and (b)
// busy-loops long enough to take timer interrupts, then reads the registers
// back and reports any that changed.

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static void load_xmm(const uint64_t* v /*32 qwords: xmm0..xmm15, 2 each*/) {
    __asm__ volatile(
        "movdqu 0x00(%0),%%xmm0\n movdqu 0x10(%0),%%xmm1\n"
        "movdqu 0x20(%0),%%xmm2\n movdqu 0x30(%0),%%xmm3\n"
        "movdqu 0x40(%0),%%xmm4\n movdqu 0x50(%0),%%xmm5\n"
        "movdqu 0x60(%0),%%xmm6\n movdqu 0x70(%0),%%xmm7\n"
        "movdqu 0x80(%0),%%xmm8\n movdqu 0x90(%0),%%xmm9\n"
        "movdqu 0xa0(%0),%%xmm10\n movdqu 0xb0(%0),%%xmm11\n"
        "movdqu 0xc0(%0),%%xmm12\n movdqu 0xd0(%0),%%xmm13\n"
        "movdqu 0xe0(%0),%%xmm14\n movdqu 0xf0(%0),%%xmm15\n"
        : : "r"(v) : "memory",
        "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
        "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
}

static void store_xmm(uint64_t* v) {
    __asm__ volatile(
        "movdqu %%xmm0,0x00(%0)\n movdqu %%xmm1,0x10(%0)\n"
        "movdqu %%xmm2,0x20(%0)\n movdqu %%xmm3,0x30(%0)\n"
        "movdqu %%xmm4,0x40(%0)\n movdqu %%xmm5,0x50(%0)\n"
        "movdqu %%xmm6,0x60(%0)\n movdqu %%xmm7,0x70(%0)\n"
        "movdqu %%xmm8,0x80(%0)\n movdqu %%xmm9,0x90(%0)\n"
        "movdqu %%xmm10,0xa0(%0)\n movdqu %%xmm11,0xb0(%0)\n"
        "movdqu %%xmm12,0xc0(%0)\n movdqu %%xmm13,0xd0(%0)\n"
        "movdqu %%xmm14,0xe0(%0)\n movdqu %%xmm15,0xf0(%0)\n"
        : : "r"(v) : "memory");
}

int main(void)
{
    uint64_t want[32], got[32];
    for (int i = 0; i < 32; ++i) want[i] = 0xA5A50000ULL + i; // distinct sentinels

    // --- Test 1: across explicit syscalls ---
    load_xmm(want);
    for (int i = 0; i < 50; ++i) { volatile long p = getpid(); (void)p; }
    store_xmm(got);
    int mismSys = 0;
    for (int i = 0; i < 32; ++i) if (got[i] != want[i]) {
        if (mismSys < 6) fprintf(stderr, "XMMTEST: SYSCALL clobber xmm%d.%d want=%#llx got=%#llx\n",
                                 i/2, i&1, (unsigned long long)want[i], (unsigned long long)got[i]);
        ++mismSys;
    }
    fprintf(stderr, "XMMTEST: syscall mismatches=%d\n", mismSys);

    // --- Test 2: across a busy loop (timer interrupts, no syscalls) ---
    load_xmm(want);
    volatile uint64_t spin = 0;
    for (uint64_t i = 0; i < 2000000000ULL; ++i) { spin += i; if (spin == 0x123456789) break; }
    store_xmm(got);
    int mismTimer = 0;
    for (int i = 0; i < 32; ++i) if (got[i] != want[i]) {
        if (mismTimer < 6) fprintf(stderr, "XMMTEST: TIMER clobber xmm%d.%d want=%#llx got=%#llx\n",
                                   i/2, i&1, (unsigned long long)want[i], (unsigned long long)got[i]);
        ++mismTimer;
    }
    fprintf(stderr, "XMMTEST: timer mismatches=%d\n", mismTimer);

    fprintf(stderr, "XMMTEST_DONE sys=%d timer=%d\n", mismSys, mismTimer);
    return 0;
}

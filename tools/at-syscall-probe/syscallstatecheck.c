#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define LOAD_X(n) "movdqu " #n "*32(%[input]), %%xmm" #n "\n\t"
#define STORE_X(n) "movdqu %%xmm" #n ", " #n "*32(%[output])\n\t"
#define LOAD_Y(n) "vmovdqu " #n "*32(%[input]), %%ymm" #n "\n\t"
#define STORE_Y(n) "vmovdqu %%ymm" #n ", " #n "*32(%[output])\n\t"
#define ALL(m) m(0) m(1) m(2) m(3) m(4) m(5) m(6) m(7) m(8) m(9) m(10) m(11) m(12) m(13) m(14) m(15)
#define SIMD_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
                     "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"

static unsigned char expected[16][32] __attribute__((aligned(32)));
static unsigned char actual[16][32] __attribute__((aligned(32)));
static int failures;

static int avx_available(void)
{
    unsigned eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if ((ecx & ((1u<<27)|(1u<<28))) != ((1u<<27)|(1u<<28))) return 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (eax & 7) == 7;
}

__attribute__((noinline))
static long capture(int avx, long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    /* All vector loads, the actual syscall instruction, and all vector
       stores are one asm statement. No C ABI call can explain a mismatch. */
    if (avx) {
        __asm__ volatile(ALL(LOAD_Y) "syscall\n\t" ALL(STORE_Y) "vzeroupper\n\t"
            : "+a"(nr), "+D"(a0), "+S"(a1), "+d"(a2), "+r"(r10), "+r"(r8), "+r"(r9)
            : [input] "r"(expected), [output] "r"(actual)
            : "rcx", "r11", "memory", "cc", SIMD_CLOBBERS);
    } else {
        __asm__ volatile(ALL(LOAD_X) "syscall\n\t" ALL(STORE_X)
            : "+a"(nr), "+D"(a0), "+S"(a1), "+d"(a2), "+r"(r10), "+r"(r8), "+r"(r9)
            : [input] "r"(expected), [output] "r"(actual)
            : "rcx", "r11", "memory", "cc", SIMD_CLOBBERS);
    }
    return nr;
}
static void report(int avx, long result, const char *label)
{
    unsigned mismatched = 0;
    for (int r = 0; r < 16; ++r)
        if (memcmp(expected[r], actual[r], avx ? 32 : 16)) mismatched |= 1u << r;
    printf("SYSCALLSTATECHECK %s %s %s result=%ld changed_register_mask=0x%04x\n",
           mismatched ? "FAIL" : "PASS", avx ? "YMM" : "XMM", label, result, mismatched);
    if (mismatched) {
        int first = __builtin_ctz(mismatched);
        for (int b = 0; b < (avx ? 32 : 16); ++b)
            if (expected[first][b] != actual[first][b]) {
                printf("SYSCALLSTATECHECK first register=%d byte=%d expected=%02x actual=%02x\n",
                       first, b, expected[first][b], actual[first][b]);
                break;
            }
    }
    failures += mismatched != 0;
    if (result < 0) {
        printf("SYSCALLSTATECHECK FAIL syscall itself returned %ld\n", result);
        ++failures;
    }
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int r = 0; r < 16; ++r)
        for (int b = 0; b < 32; ++b) expected[r][b] = 1 + ((r*37 + b*13) % 255);
    int pipefd[2];
    if (pipe(pipefd)) { perror("pipe"); return 2; }
    unsigned char buffer[64] = {0};
    struct timespec clock_value, delay = {0, 2000000};
    int have_avx = avx_available();
    for (int avx = 0; avx <= have_avx; ++avx) {
        long result = capture(avx, SYS_getpid, 0, 0, 0, 0, 0, 0);
        report(avx, result, "getpid");
        result = capture(avx, SYS_mmap, 0, 16384, PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        report(avx, result, "anonymous mmap zero-fill");
        if (result > 0) munmap((void *)result, 16384);
        long old_break = syscall(SYS_brk, 0);
        result = capture(avx, SYS_brk, old_break+4096, 0, 0, 0, 0, 0);
        /* Restore before any libc call can allocate using the temporary break. */
        syscall(SYS_brk, old_break);
        report(avx, result, "brk growth");
        result = capture(avx, SYS_write, pipefd[1], (long)buffer, sizeof(buffer), 0, 0, 0);
        report(avx, result, "pipe write");
        if (result == sizeof(buffer)) {
            result = capture(avx, SYS_read, pipefd[0], (long)buffer, sizeof(buffer), 0, 0, 0);
            report(avx, result, "pipe read");
        }
        result = capture(avx, SYS_clock_gettime, CLOCK_MONOTONIC, (long)&clock_value, 0, 0, 0, 0);
        report(avx, result, "clock_gettime");
        result = capture(avx, SYS_nanosleep, (long)&delay, 0, 0, 0, 0, 0);
        report(avx, result, "nanosleep with context switch");
    }
    if (!have_avx) puts("SYSCALLSTATECHECK SKIP YMM: CPU/OS AVX unavailable");
    close(pipefd[0]); close(pipefd[1]);
    printf("SYSCALLSTATECHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

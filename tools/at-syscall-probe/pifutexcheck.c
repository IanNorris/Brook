// Raw PI-futex capability regression. Linux control and disposable Brook guest.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static unsigned checks;
#define CK(x) do { ++checks; if (!(x)) { fprintf(stderr, "PIFUTEX FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static long fx(uint32_t* word, int op) {
    errno = 0;
    return syscall(SYS_futex, word, op, 0, NULL, word, 0);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    uint32_t tid = (uint32_t)syscall(SYS_gettid), word = tid;
    if (!strcmp(argv[1], "--observe")) {
        long lock = fx(&word, 6 | 128); int lock_errno = errno;
        uint32_t after = word; word = 0;
        long unlock = fx(&word, 7 | 128); int unlock_errno = errno;
        printf("PI_OBSERVE lock_self=%ld errno=%d owner_preserved=%d unlock_unowned=%ld errno=%d\n",
               lock, lock_errno, after == tid, unlock, unlock_errno);
        return 0;
    }
    if (!strcmp(argv[1], "--expect-unsupported")) {
        const int ops[] = {6, 7, 8, 11, 12, 13};
        const int flags[] = {0, 128, 256, 128 | 256};
        const uint32_t values[] = {0, tid, tid + 1, 0x80000000u | tid};
        for (unsigned o = 0; o < sizeof(ops)/sizeof(*ops); ++o)
            for (unsigned f = 0; f < sizeof(flags)/sizeof(*flags); ++f) {
                for (unsigned v = 0; v < sizeof(values)/sizeof(*values); ++v) {
                    word = values[v];
                    CK(fx(&word, ops[o] | flags[f]) == -1 && errno == ENOSYS);
                    CK(word == values[v]);
                }
                CK(fx(NULL, ops[o] | flags[f]) == -1 && errno == ENOSYS);
            }
    } else if (!strcmp(argv[1], "--expect-supported")) {
        for (int flags = 0; flags <= 128; flags += 128) {
            word = tid;
            CK(fx(&word, 6 | flags) == -1 && errno == EDEADLK);
            CK(word == tid);
            word = 0;
            CK(fx(&word, 7 | flags) == -1 && errno == EPERM);
            CK(word == 0);
            CK(fx(&word, 6 | flags) == 0 && word == tid);
            CK(fx(&word, 7 | flags) == 0 && word == 0);
        }
    } else return 2;
    // Ordinary futex capability must survive rejection of unsupported PI ops.
    word = 7;
    CK(fx(&word, 128 /* WAIT_PRIVATE, expected 0 */) == -1 && errno == EAGAIN);
    CK(word == 7);
    CK(fx(&word, 1 | 128 /* WAKE_PRIVATE, zero waiters */) == 0);
    printf("PIFUTEX_PASS checks=%u mode=%s\n", checks, argv[1]);
    return 0;
}

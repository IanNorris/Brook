// futexstress — reproduce BRO-205: sys_futex returning a glibc-fatal errno
// (-EFAULT/-EINVAL) for a FUTEX_WAIT on a valid futex word, under heavy
// concurrent page-table churn (mmap/munmap/brk) across all CPUs.
//
// glibc's pthread_cond_wait calls futex_fatal_error() ("The futex facility
// returned an unexpected error code") and abort()s if FUTEX_WAIT returns
// anything other than 0, EAGAIN, EINTR or ETIMEDOUT. This test issues raw
// FUTEX_WAIT syscalls (like glibc's condvar) from many waiter threads while
// churner threads hammer the page tables, and flags any unexpected return.
//
// Deterministic pass/fail: prints "FUTEXSTRESS: PASS" if no waiter ever saw an
// unexpected errno, or "FUTEXSTRESS: FAIL ..." with the offending errno the
// first time one occurs.

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 202
#endif
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

#define NWAIT 16   // prober threads (hammer FUTEX_WAIT entry / page-walk)
#define NCHURN 8   // page-table churn threads
#define NWAKE 2    // waker threads
#define RUN_SECONDS 40

// Stable, never-freed futex words the waiters block on.
static _Atomic uint32_t *g_words;       // NWAIT words on a dedicated page
static _Atomic int g_stop = 0;
static _Atomic long g_wait_calls = 0;
static _Atomic long g_bad = 0;          // count of unexpected errno returns
static _Atomic int g_bad_errno = 0;     // first offending errno
static _Atomic int g_bad_op = 0;

static long sys_futex(_Atomic uint32_t *uaddr, int op, uint32_t val,
                      const struct timespec *to) {
    return syscall(SYS_futex, uaddr, op, val, to, NULL, FUTEX_BITSET_MATCH_ANY);
}

static void note_bad(int op, int err) {
    if (atomic_fetch_add(&g_bad, 1) == 0) {
        atomic_store(&g_bad_errno, err);
        atomic_store(&g_bad_op, op);
        // Loud, unbuffered — this is the smoking gun.
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
                         "FUTEXSTRESS: BAD futex return op=%d errno=%d (%s)\n",
                         op, err, strerror(err));
        if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
    }
}

// Prober: hammer the kernel's FUTEX_WAIT entry path (which runs the
// UserBufferReadable page-table walk on the futex word) as fast as possible by
// passing a deliberately-wrong expected value, so FUTEX_WAIT validates the
// pointer and returns EAGAIN immediately without blocking. Millions of probes
// per second race against the churn threads' page-table teardown — the window
// in which a valid word can be transiently seen as unmapped (-> -EFAULT). Also
// runs real blocking waits (bitset, 1ms) to exercise the timeout path.
static void *waiter_fn(void *arg) {
    long idx = (long)arg;
    _Atomic uint32_t *w = &g_words[idx % NWAIT];
    while (!atomic_load(&g_stop)) {
        // Fast non-blocking probe: expected value can't match -> EAGAIN.
        uint32_t cur = atomic_load(w);
        long r = syscall(SYS_futex, w, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                         cur + 0x40000000u, NULL, NULL, 0);
        atomic_fetch_add(&g_wait_calls, 1);
        if (r != 0) {
            int e = errno;
            if (e != EAGAIN && e != EINTR && e != ETIMEDOUT)
                note_bad(FUTEX_WAIT, e);
        }
        // Occasionally a real timed wait (bitset + absolute monotonic).
        if ((idx & 3) == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += 1 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            cur = atomic_load(w);
            r = syscall(SYS_futex, w, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                        cur, &ts, NULL, FUTEX_BITSET_MATCH_ANY);
            atomic_fetch_add(&g_wait_calls, 1);
            if (r != 0) {
                int e = errno;
                if (e != EAGAIN && e != EINTR && e != ETIMEDOUT)
                    note_bad(FUTEX_WAIT_BITSET, e);
            }
        }
    }
    return NULL;
}

// Waker: flip the word and wake, so waiters churn through WAIT/EAGAIN/wake.
static void *waker_fn(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        for (int i = 0; i < NWAIT; i++) {
            atomic_fetch_add(&g_words[i], 1);
            sys_futex(&g_words[i], FUTEX_WAKE | FUTEX_PRIVATE_FLAG, INT32_MAX, NULL);
        }
        // No sleep: wake as fast as possible so waiters re-enter FUTEX_WAIT
        // (and thus the kernel's UserBufferReadable page-table walk) millions
        // of times, maximizing the chance of racing a churn thread's munmap.
    }
    return NULL;
}

// Churn: hammer the page tables (mmap+touch+munmap) and the brk heap, to
// stress the lockless page-table walk in the kernel's user-pointer checks and
// force TLB shootdowns across CPUs — the conditions under which a FUTEX_WAIT on
// a *valid* word can spuriously see the page as unmapped.
static void *churn_fn(void *arg) {
    (void)arg;
    const size_t sz = 4 * 1024 * 1024; // 4MB = two 2MB page-table spans
    while (!atomic_load(&g_stop)) {
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            // Touch one page per 2MB span so the kernel allocates the leaf
            // page table, then munmap frees it — that PT alloc/free is the
            // teardown a concurrent futex page-walk can race.
            for (size_t off = 0; off < sz; off += 2 * 1024 * 1024)
                ((volatile char *)p)[off] = 1;
            munmap(p, sz);
        }
        // brk churn too.
        void *h = sbrk(128 * 1024);
        if (h != (void *)-1) {
            ((volatile char *)h)[0] = 2;
            sbrk(-128 * 1024);
        }
    }
    return NULL;
}

int main(void) {
    g_words = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_words == MAP_FAILED) {
        write(2, "FUTEXSTRESS: mmap failed\n", 25);
        return 1;
    }
    for (int i = 0; i < NWAIT; i++) atomic_store(&g_words[i], 0);

    printf("FUTEXSTRESS: start waiters=%d churn=%d wakers=%d for %ds\n",
           NWAIT, NCHURN, NWAKE, RUN_SECONDS);
    fflush(stdout);

    pthread_t wt[NWAIT], ct[NCHURN], kt[NWAKE];
    for (long i = 0; i < NWAIT; i++) pthread_create(&wt[i], NULL, waiter_fn, (void *)i);
    for (long i = 0; i < NCHURN; i++) pthread_create(&ct[i], NULL, churn_fn, NULL);
    for (long i = 0; i < NWAKE; i++) pthread_create(&kt[i], NULL, waker_fn, NULL);

    for (int s = 0; s < RUN_SECONDS; s++) {
        struct timespec t = {1, 0};
        nanosleep(&t, NULL);
        if (atomic_load(&g_bad)) break; // stop early on first failure
    }
    atomic_store(&g_stop, 1);

    // Wake everyone so waiters unblock and threads can join.
    for (int i = 0; i < NWAIT; i++) {
        atomic_fetch_add(&g_words[i], 1);
        sys_futex(&g_words[i], FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1000, NULL);
    }
    for (int i = 0; i < NWAIT; i++) pthread_join(wt[i], NULL);
    for (int i = 0; i < NCHURN; i++) pthread_join(ct[i], NULL);
    for (int i = 0; i < NWAKE; i++) pthread_join(kt[i], NULL);

    long bad = atomic_load(&g_bad);
    printf("FUTEXSTRESS: wait_calls=%ld bad=%ld\n",
           atomic_load(&g_wait_calls), bad);
    if (bad) {
        printf("FUTEXSTRESS: FAIL op=%d errno=%d (%s) — this is what makes "
               "glibc abort with 'futex facility returned an unexpected error'\n",
               atomic_load(&g_bad_op), atomic_load(&g_bad_errno),
               strerror(atomic_load(&g_bad_errno)));
        fflush(stdout);
        return 2;
    }
    printf("FUTEXSTRESS: PASS\n");
    fflush(stdout);
    return 0;
}

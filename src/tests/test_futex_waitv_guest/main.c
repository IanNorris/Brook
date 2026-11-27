// QEMU guest test for futex_waitv (449) and RLIMIT_NOFILE, the two kernel
// features Proton/Wine fsync and esync depend on. Prints FUTEX_WAITV_OK or
// FUTEX_WAITV_FAIL on stdout (the serial log).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX2_SIZE_U8 0
#define FUTEX2_SIZE_U16 1
#define FUTEX2_SIZE_U32 2
#define FUTEX2_SIZE_U64 3
#define FUTEX2_PRIVATE 128

struct waitv { uint64_t val, uaddr; uint32_t flags, reserved; };

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FUTEX_WAITV_CHECK_FAIL line %d: %s: ", __LINE__, #cond); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

static long waitv(struct waitv* w, unsigned n, const struct timespec* abs, int clock)
{
    long r = syscall(SYS_futex_waitv, w, n, 0, abs, clock);
    return r < 0 ? -errno : r;
}
static long fwake(volatile uint32_t* a, int n, int priv)
{
    long r = syscall(SYS_futex, a, FUTEX_WAKE | (priv ? FUTEX_PRIVATE_FLAG : 0), n, 0, 0, 0);
    return r < 0 ? -errno : r;
}
static void setw(struct waitv* w, volatile uint32_t* a, uint32_t v, int priv)
{
    w->val = v; w->uaddr = (uintptr_t)a; w->flags = FUTEX2_SIZE_U32 | (priv ? FUTEX2_PRIVATE : 0); w->reserved = 0;
}
static uint64_t now_ns(int clock)
{
    struct timespec t; clock_gettime(clock, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static struct timespec after_ms(int clock, uint64_t ms)
{
    uint64_t t = now_ns(clock) + ms * 1000000ull;
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    return ts;
}
static void sleep_ms(unsigned ms) { struct timespec t = {0, (long)ms * 1000000L}; nanosleep(&t, 0); }

// ---------------------------------------------------------------- argument checks
static void test_arguments(void)
{
    static uint32_t word = 7;
    struct waitv w[129];
    for (int i = 0; i < 129; ++i) setw(&w[i], &word, 7, 1);
    // Proton's probe: futex_waitv(NULL, 0, 0, NULL, 0) must not be ENOSYS.
    long r = syscall(SYS_futex_waitv, 0, 0, 0, 0, 0);
    CHECK(r == -1 && errno == EINVAL, "probe r=%ld errno=%d", r, errno);
    CHECK(waitv(w, 0, 0, CLOCK_MONOTONIC) == -EINVAL, "count 0");
    CHECK(waitv(w, 129, 0, CLOCK_MONOTONIC) == -EINVAL, "count 129");
    r = syscall(SYS_futex_waitv, w, 1, 1, 0, CLOCK_MONOTONIC);
    CHECK(r == -1 && errno == EINVAL, "flags");
    struct timespec ts = after_ms(CLOCK_MONOTONIC, 10);
    CHECK(waitv(w, 1, &ts, CLOCK_BOOTTIME) == -EINVAL, "clock");
    struct timespec bad = {0, 1000000000L};
    CHECK(waitv(w, 1, &bad, CLOCK_MONOTONIC) == -EINVAL, "bad nsec");
    unsigned sizes[] = {FUTEX2_SIZE_U8, FUTEX2_SIZE_U16, FUTEX2_SIZE_U64};
    for (int i = 0; i < 3; ++i) {
        struct waitv x = w[0]; x.flags = sizes[i];
        CHECK(waitv(&x, 1, 0, CLOCK_MONOTONIC) == -EINVAL, "size %u", sizes[i]);
    }
    struct waitv x = w[0]; x.reserved = 1;
    CHECK(waitv(&x, 1, 0, CLOCK_MONOTONIC) == -EINVAL, "reserved");
    x = w[0]; x.uaddr += 1;
    CHECK(waitv(&x, 1, 0, CLOCK_MONOTONIC) == -EINVAL, "misaligned");
    x = w[0]; x.val = 0x100000007ull;
    CHECK(waitv(&x, 1, 0, CLOCK_MONOTONIC) == -EINVAL, "val > u32");
    x = w[0]; x.uaddr = 0x10;
    CHECK(waitv(&x, 1, 0, CLOCK_MONOTONIC) == -EFAULT, "unmapped");
    CHECK(waitv((struct waitv*)0x10, 1, 0, CLOCK_MONOTONIC) == -EFAULT, "unmapped vector");
    printf("FUTEX_WAITV_ARGS done\n");
}

// ---------------------------------------------------------------- EAGAIN and timeout
static void test_mismatch_timeout(void)
{
    static uint32_t a[128];
    struct waitv w[128];
    for (int i = 0; i < 128; ++i) { a[i] = (uint32_t)i; setw(&w[i], &a[i], (uint32_t)i, i & 1); }
    for (int k = 0; k < 128; k += 9) {
        w[k].val = 1000;
        long r = waitv(w, 128, 0, CLOCK_MONOTONIC);
        CHECK(r == -EAGAIN, "mismatch at %d r=%ld", k, r);
        w[k].val = (uint32_t)k;
    }
    const int clocks[] = {CLOCK_MONOTONIC, CLOCK_REALTIME};
    for (int c = 0; c < 2; ++c) {
        uint64_t start = now_ns(clocks[c]);
        struct timespec ts = after_ms(clocks[c], 60);
        long r = waitv(w, 128, &ts, clocks[c]);
        uint64_t elapsed = now_ns(clocks[c]) - start;
        CHECK(r == -ETIMEDOUT, "clock %d r=%ld", clocks[c], r);
        CHECK(elapsed >= 59000000ull && elapsed < 2000000000ull, "clock %d elapsed %llu", clocks[c],
              (unsigned long long)elapsed);
    }
    struct timespec past = after_ms(CLOCK_MONOTONIC, 0);
    past.tv_sec -= 1;
    CHECK(waitv(w, 4, &past, CLOCK_MONOTONIC) == -ETIMEDOUT, "past deadline");
    w[2].val = 99;
    CHECK(waitv(w, 4, &past, CLOCK_MONOTONIC) == -EAGAIN, "mismatch wins over expired");
    printf("FUTEX_WAITV_EAGAIN_TIMEOUT done\n");
}

// ---------------------------------------------------------------- index and threads
struct idx_arg { volatile uint32_t* words; unsigned n; int priv; volatile int ready; long result; };
static void* idx_waiter(void* p)
{
    struct idx_arg* a = p;
    struct waitv w[128];
    for (unsigned i = 0; i < a->n; ++i) setw(&w[i], &a->words[i], 0, a->priv);
    a->ready = 1;
    struct timespec ts = after_ms(CLOCK_MONOTONIC, 5000);
    a->result = waitv(w, a->n, &ts, CLOCK_MONOTONIC);
    return 0;
}
static void test_index(void)
{
    static uint32_t words[128];
    const unsigned counts[] = {1, 2, 8, 128};
    for (int c = 0; c < 4; ++c) {
        unsigned n = counts[c];
        for (unsigned k = 0; k < n; k = k ? k * 3 + 1 : 1) {
            for (int priv = 0; priv < 2; ++priv) {
                memset(words, 0, sizeof(words));
                struct idx_arg a = {words, n, priv, 0, -999};
                pthread_t t; pthread_create(&t, 0, idx_waiter, &a);
                while (!a.ready) sched_yield();
                sleep_ms(2);
                __atomic_store_n(&words[k], 1, __ATOMIC_SEQ_CST);
                long woke = 0;
                // The waiter may not be queued yet; it then sees EAGAIN. Wake
                // until it leaves (classic futex wake on the same key).
                for (int tries = 0; tries < 2000 && a.result == -999; ++tries) {
                    long r = fwake(&words[k], 1, priv);
                    if (r > 0) woke += r;
                    sleep_ms(1);
                }
                pthread_join(t, 0);
                CHECK(a.result == (long)k || a.result == -EAGAIN, "n=%u k=%u priv=%d result=%ld", n, k, priv, a.result);
                CHECK(a.result != -EAGAIN || woke == 0, "EAGAIN but %ld woken", woke);
            }
            if (n == 1) break;
        }
    }
    printf("FUTEX_WAITV_INDEX done\n");
}

// Several waiters, each on {own, shared}: one FUTEX_WAKE(shared, INT_MAX)
// releases all of them with index 1; FUTEX_WAKE(own) releases one with 0.
struct multi_arg { volatile uint32_t* own; volatile uint32_t* shared; volatile int* queued; long result; };
static void* multi_waiter(void* p)
{
    struct multi_arg* a = p;
    struct waitv w[2]; setw(&w[0], a->own, 0, 0); setw(&w[1], a->shared, 0, 0);
    __atomic_fetch_add(a->queued, 1, __ATOMIC_SEQ_CST);
    struct timespec ts = after_ms(CLOCK_MONOTONIC, 5000);
    a->result = waitv(w, 2, &ts, CLOCK_MONOTONIC);
    return 0;
}
static void test_multi_waiters(void)
{
    enum { N = 8 };
    static uint32_t own[N], shared;
    volatile int queued = 0;
    struct multi_arg args[N]; pthread_t t[N];
    for (int i = 0; i < N; ++i) { args[i] = (struct multi_arg){&own[i], &shared, &queued, -999}; pthread_create(&t[i], 0, multi_waiter, &args[i]); }
    while (queued < N) sched_yield();
    sleep_ms(20);
    // Release waiter 0 through its own word first.
    long r = fwake(&own[0], 1, 0);
    CHECK(r == 1, "own wake r=%ld", r);
    pthread_join(t[0], 0);
    CHECK(args[0].result == 0, "own result %ld", args[0].result);
    r = fwake(&shared, INT_MAX, 0);
    CHECK(r == N - 1, "shared wake woke %ld", r);
    for (int i = 1; i < N; ++i) { pthread_join(t[i], 0); CHECK(args[i].result == 1, "waiter %d result %ld", i, args[i].result); }
    printf("FUTEX_WAITV_MULTI done\n");
}

// /proc/<pid>/futex shows a blocked waiter's expected and live value.
struct proc_arg { volatile uint32_t* word; volatile int ready; volatile pid_t tid; long result; };
static void* proc_waiter(void* p)
{
    struct proc_arg* a = p;
    struct waitv w; setw(&w, a->word, 0, 0);
    a->tid = (pid_t)syscall(SYS_gettid);
    a->ready = 1;
    struct timespec ts = after_ms(CLOCK_MONOTONIC, 5000);
    a->result = waitv(&w, 1, &ts, CLOCK_MONOTONIC);
    return 0;
}
static int read_proc_futex(pid_t pid, char* buf, size_t cap)
{
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/futex", (int)pid);
    int fd = open(path, O_RDONLY); if (fd < 0) return -errno;
    ssize_t n = read(fd, buf, cap - 1); close(fd);
    buf[n > 0 ? n : 0] = 0; return (int)n;
}
static void test_proc_futex(void)
{
    int fd = memfd_create("procfutex", 0);
    CHECK(fd >= 0 && ftruncate(fd, 4096) == 0, "memfd");
    volatile uint32_t* word = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct proc_arg a = {word + 3, 0, 0, -999};
    pthread_t t; pthread_create(&t, 0, proc_waiter, &a);
    while (!a.ready) sched_yield();
    sleep_ms(30);
    char buf[2048];
    int n = read_proc_futex(a.tid, buf, sizeof(buf));
    printf("FUTEX_WAITV_PROC before: %s", n > 0 ? buf : "(none)\n");
    CHECK(n > 0 && strstr(buf, "waitv") && strstr(buf, "expected=0 current=0") && strstr(buf, "key=memfd:") &&
          (strstr(buf, "+0xc") || strstr(buf, "+0xC")), "proc futex row: %s", buf);
    __atomic_store_n(a.word, 5, __ATOMIC_SEQ_CST); // changed, deliberately not woken
    n = read_proc_futex(getpid(), buf, sizeof(buf));  // thread-group view
    printf("FUTEX_WAITV_PROC changed: %s", n > 0 ? buf : "(none)\n");
    CHECK(n > 0 && strstr(buf, "expected=0 current=5"), "proc futex current: %s", buf);
    fwake(a.word, 1, 0);
    pthread_join(t, 0);
    CHECK(a.result == 0, "proc waiter result %ld", a.result);
    n = read_proc_futex(a.tid, buf, sizeof(buf));
    CHECK(n <= 0 || !strstr(buf, "waitv"), "stale row after wake: %s", buf);
    munmap((void*)word, 4096); close(fd);
    printf("FUTEX_WAITV_PROC done\n");
}

// ---------------------------------------------------------------- signals
static volatile int g_handled;
static void on_usr1(int s) { (void)s; g_handled++; }
struct sig_arg { volatile uint32_t* word; volatile int ready; long result; };
static void* sig_waiter(void* p)
{
    struct sig_arg* a = p;
    struct waitv w[3]; static uint32_t pad[2];
    setw(&w[0], &pad[0], 0, 1); setw(&w[1], &pad[1], 0, 0); setw(&w[2], a->word, 0, 0);
    a->ready = 1;
    struct timespec ts = after_ms(CLOCK_MONOTONIC, 4000);
    a->result = waitv(w, 3, &ts, CLOCK_MONOTONIC);
    return 0;
}
static void test_signal(void)
{
    static uint32_t word;
    for (int restart = 0; restart < 2; ++restart) {
        struct sigaction sa; memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1; sa.sa_flags = restart ? SA_RESTART : 0;
        sigaction(SIGUSR1, &sa, 0);
        g_handled = 0; word = 0;
        struct sig_arg a = {&word, 0, -999};
        pthread_t t; pthread_create(&t, 0, sig_waiter, &a);
        while (!a.ready) sched_yield();
        sleep_ms(30);
        pthread_kill(t, SIGUSR1);
        if (restart) {
            sleep_ms(50);
            CHECK(a.result == -999, "SA_RESTART returned early %ld", a.result);
            word = 1; fwake(&word, 1, 0);
            for (int i = 0; i < 1000 && a.result == -999; ++i) { fwake(&word, 1, 0); sleep_ms(1); }
        }
        pthread_join(t, 0);
        CHECK(g_handled == 1, "handler ran %d", g_handled);
        if (restart) CHECK(a.result == 2 || a.result == -EAGAIN, "restart result %ld", a.result);
        else CHECK(a.result == -EINTR, "EINTR result %ld", a.result);
    }
    signal(SIGUSR1, SIG_DFL);
    printf("FUTEX_WAITV_SIGNAL done\n");
}

// ---------------------------------------------------------------- cross-process
// Shared page layout: [0] child ready, [1] parent->child word, [2] child->parent word.
static int child_wait(volatile uint32_t* page, int index_of_target)
{
    static uint32_t local[4];
    struct waitv w[4];
    for (int i = 0; i < 4; ++i) setw(&w[i], &local[i], 0, 1);
    setw(&w[index_of_target], &page[1], 0, 0);
    __atomic_store_n(&page[0], 1, __ATOMIC_SEQ_CST);
    fwake(&page[0], 1, 0);
    struct timespec ts = after_ms(CLOCK_MONOTONIC, 5000);
    long r;
    do r = waitv(w, 4, &ts, CLOCK_MONOTONIC);
    while (r == -EAGAIN && page[1] == 0);
    if (r == -EAGAIN) r = index_of_target; // value already changed: wake raced the setup
    // Now wake the parent waiting on page[2] in its own mapping.
    __atomic_store_n(&page[2], 1, __ATOMIC_SEQ_CST);
    fwake(&page[2], 1, 0);
    return r == index_of_target ? 0 : (r < 0 ? 100 + (int)-r : 50 + (int)r);
}
static int parent_side(volatile uint32_t* page, pid_t child, const char* name)
{
    struct timespec ts = after_ms(CLOCK_MONOTONIC, 5000);
    while (!__atomic_load_n(&page[0], __ATOMIC_SEQ_CST)) {
        syscall(SYS_futex, &page[0], FUTEX_WAIT, 0, 0, 0, 0);
        if (now_ns(CLOCK_MONOTONIC) > (uint64_t)ts.tv_sec * 1000000000ull) break;
    }
    sleep_ms(20);
    __atomic_store_n(&page[1], 1, __ATOMIC_SEQ_CST);
    long woke = fwake(&page[1], 1, 0);
    // The parent waits through futex_waitv on its own (different) mapping.
    struct waitv w[2]; static uint32_t pad;
    setw(&w[0], &pad, 0, 1); setw(&w[1], &page[2], 0, 0);
    long r;
    do r = waitv(w, 2, &ts, CLOCK_MONOTONIC);
    while (r == -EAGAIN && page[2] == 0);
    int status = 0; waitpid(child, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
    CHECK(code == 0, "%s child exit %d (woke=%ld)", name, code, woke);
    CHECK(r == 1 || (r == -EAGAIN && page[2] == 1), "%s parent waitv %ld", name, r);
    return code;
}
static void test_cross_process(void)
{
    // MAP_SHARED anonymous, inherited across fork.
    volatile uint32_t* anon = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(anon != MAP_FAILED, "anon mmap");
    pid_t pid = fork();
    if (!pid) _exit(child_wait(anon, 2));
    parent_side(anon, pid, "anon");

    // memfd: the child maps it again at another address (different VA, same key).
    int fd = memfd_create("waitv", 0);
    CHECK(fd >= 0 && ftruncate(fd, 8192) == 0, "memfd");
    volatile uint32_t* mem = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    volatile uint32_t* memPage = (volatile uint32_t*)((char*)mem + 4096);
    pid = fork();
    if (!pid) {
        void* spacer = mmap(0, 65536, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); (void)spacer;
        volatile uint32_t* again = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
        if (again == MAP_FAILED || again == memPage) _exit(90);
        _exit(child_wait(again, 3));
    }
    parent_side(memPage, pid, "memfd");

    // POSIX shm (Wine's fsync shm): each process opens the name separately.
    char name[64];
    snprintf(name, sizeof(name), "/brook-futex-waitv-%d", (int)getpid()); // concurrent copies
    shm_unlink(name);
    int sfd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(sfd >= 0 && ftruncate(sfd, 4096) == 0, "shm_open");
    volatile uint32_t* shm = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    pid = fork();
    if (!pid) {
        close(sfd);
        int cfd = shm_open(name, O_RDWR, 0);
        void* spacer = mmap(0, 65536, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); (void)spacer;
        volatile uint32_t* mine = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
        if (cfd < 0 || mine == MAP_FAILED) _exit(91);
        _exit(child_wait(mine, 0));
    }
    parent_side(shm, pid, "shm");
    shm_unlink(name);
    printf("FUTEX_WAITV_CROSS_PROCESS done\n");
}

// A waiter killed while blocked never returns to unqueue its nodes, which
// live on its kernel stack: process cancellation must unlink every one before
// that stack can be reaped. Afterwards no node may remain on the shared words.
static void* exit_group_waiter(void* p)
{
    volatile uint32_t* page = p;
    struct waitv w[128];
    for (int i = 0; i < 128; ++i) setw(&w[i], &page[64 + (i % 8)], 0, 0);
    __atomic_fetch_add(&page[1], 1, __ATOMIC_SEQ_CST);
    waitv(w, 128, 0, CLOCK_MONOTONIC);
    return 0;
}
static void test_killed_waiters(void)
{
    volatile uint32_t* page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    for (int round = 0; round < 40; ++round) {
        memset((void*)page, 0, 4096);
        pid_t pid = fork();
        if (!pid) {
            // Threads blocked in futex_waitv while the main thread exits the group
            // (even rounds) or the parent SIGKILLs the group (odd rounds).
            pthread_t t[3];
            for (int i = 0; i < 3; ++i) pthread_create(&t[i], 0, exit_group_waiter, (void*)page);
            while (page[1] < 3) sched_yield();
            sleep_ms(5);
            if (!(round & 1)) _exit(0);
            exit_group_waiter((void*)page);
            _exit(1);
        }
        while (page[1] < 3u + (unsigned)(round & 1)) sched_yield();
        sleep_ms(5);
        if (round & 1) kill(pid, SIGKILL);
        int status = 0; waitpid(pid, &status, 0);
        long stale = 0;
        for (int i = 0; i < 8; ++i) { long r = fwake(&page[64 + i], INT_MAX, 0); if (r > 0) stale += r; }
        CHECK(stale == 0, "round %d: %ld nodes left after the waiter group died", round, stale);
    }
    printf("FUTEX_WAITV_KILLED done\n");
}

// ---------------------------------------------------------------- lost-wakeup stress
// Ping-pong on one turn word. The waiter side uses futex_waitv with the turn
// word at a rotating index among decoys; the other side waits with classic
// FUTEX_WAIT. A lost wakeup shows up as a 2 s timeout while the turn word
// already says "your turn".
struct pp { volatile uint32_t* turn; uint32_t iterations; int priv; long lost, errors, slept, again; };
// Vary the hand-off delay so the wake lands before, inside and after the
// waiter's queue-then-check window.
static void spin(uint32_t i) { for (volatile uint32_t n = (i * 2654435761u) >> 22; n; --n) {} }
static void* pp_classic(void* p)
{
    struct pp* a = p;
    for (uint32_t i = 0; i < a->iterations; ++i) {
        while (__atomic_load_n(a->turn, __ATOMIC_ACQUIRE) != 1) {
            struct timespec rel = {2, 0};
            long r = syscall(SYS_futex, a->turn, FUTEX_WAIT | (a->priv ? FUTEX_PRIVATE_FLAG : 0), 0, &rel, 0, 0);
            if (r < 0 && errno == ETIMEDOUT && __atomic_load_n(a->turn, __ATOMIC_ACQUIRE) == 1) a->lost++;
        }
        spin(i);
        __atomic_store_n(a->turn, 0, __ATOMIC_RELEASE);
        fwake(a->turn, 1, a->priv);
    }
    return 0;
}
static void pp_waitv(struct pp* a)
{
    static uint32_t decoys[8];
    struct waitv w[8];
    for (uint32_t i = 0; i < a->iterations; ++i) {
        const unsigned n = 1 + (i % 8), k = (i * 7) % n;
        for (unsigned j = 0; j < n; ++j) setw(&w[j], &decoys[j], 0, j & 1);
        setw(&w[k], a->turn, 1, a->priv);
        while (__atomic_load_n(a->turn, __ATOMIC_ACQUIRE) != 0) {
            struct timespec ts = after_ms(CLOCK_MONOTONIC, 2000);
            long r = waitv(w, n, &ts, CLOCK_MONOTONIC);
            if (r == -ETIMEDOUT && __atomic_load_n(a->turn, __ATOMIC_ACQUIRE) == 0) a->lost++;
            else if (r >= 0 && r != (long)k) a->errors++;
            else if (r >= 0) a->slept++;
            else if (r == -EAGAIN) a->again++;
            else if (r != -ETIMEDOUT) a->errors++;
        }
        spin(i ^ 0x5bd1e995u);
        __atomic_store_n(a->turn, 1, __ATOMIC_RELEASE);
        fwake(a->turn, 1, a->priv);
    }
}
static void test_stress(uint32_t iterations)
{
    static uint32_t turn;
    for (int priv = 0; priv < 2; ++priv) {
        turn = 0;
        struct pp a = {&turn, iterations / 2, priv, 0, 0, 0, 0};
        uint64_t start = now_ns(CLOCK_MONOTONIC);
        pthread_t t; pthread_create(&t, 0, pp_classic, &a);
        pp_waitv(&a);
        pthread_join(t, 0);
        printf("FUTEX_WAITV_STRESS threads priv=%d iterations=%u slept=%ld eagain=%ld lost=%ld errors=%ld ms=%llu\n", priv,
               a.iterations, a.slept, a.again, a.lost, a.errors,
               (unsigned long long)((now_ns(CLOCK_MONOTONIC) - start) / 1000000ull));
        CHECK(a.lost == 0 && a.errors == 0, "thread stress priv=%d lost=%ld errors=%ld", priv, a.lost, a.errors);
    }
    // Cross-process over MAP_SHARED anonymous memory.
    volatile uint32_t* page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    struct pp* shared = (struct pp*)((char*)page + 64);
    *shared = (struct pp){page, iterations / 4, 0, 0, 0, 0, 0};
    uint64_t start = now_ns(CLOCK_MONOTONIC);
    pid_t pid = fork();
    if (!pid) { pp_classic(shared); _exit(0); }
    pp_waitv(shared);
    int status = 0; waitpid(pid, &status, 0);
    printf("FUTEX_WAITV_STRESS process iterations=%u slept=%ld eagain=%ld lost=%ld errors=%ld ms=%llu\n",
           shared->iterations, shared->slept, shared->again, shared->lost, shared->errors,
           (unsigned long long)((now_ns(CLOCK_MONOTONIC) - start) / 1000000ull));
    CHECK(WIFEXITED(status) && shared->lost == 0 && shared->errors == 0, "process stress lost=%ld errors=%ld",
          shared->lost, shared->errors);
}

// ---------------------------------------------------------------- Wine fsync model
// Mirrors Proton's dlls/ntdll/unix/fsync.c + server/fsync.c: the server
// creates "/wine-...-fsync" with shm_open(O_CREAT|O_EXCL), sizes it with
// ftruncate one 64 KiB page at a time as object indices grow, and every
// process maps each page lazily and separately (mmap(NULL, page, RW,
// MAP_SHARED, fd, entry * page)). Waits are futex_waitv(nr=1, FUTEX_32,
// non-private); wakes are non-private FUTEX_WAKE(INT_MAX). Server, waiter
// and waker are separate exec'd programs, optionally in a mount namespace
// where /dev/shm is a bind mount, as under Steam's pressure-vessel.
#define FSYNC_PAGE 0x10000
#define FSYNC_ROUNDS 240
struct fsync_ctl { uint32_t round, idx, ready, done, failures, lost, unsignaled, woken; };
struct fsync_map { int fd; void* pages[256]; };
static volatile uint32_t* fsync_obj(struct fsync_map* m, uint32_t idx)
{
    uint32_t entry = idx * 16 / FSYNC_PAGE, offset = idx * 16 % FSYNC_PAGE;
    if (entry >= 256) return 0;
    if (!m->pages[entry]) {
        void* a = mmap(0, FSYNC_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, (off_t)entry * FSYNC_PAGE);
        if (a == MAP_FAILED) return 0;
        // Separate spacing so every process maps each page at its own address.
        void* spacer = mmap(0, 4096 * (1 + (getpid() % 7)), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); (void)spacer;
        m->pages[entry] = a;
    }
    return (volatile uint32_t*)((char*)m->pages[entry] + offset);
}
static long fsync_wait_value(volatile uint32_t* word, uint32_t current, unsigned ms)
{
    struct waitv w; setw(&w, word, current, 0);
    struct timespec end = after_ms(CLOCK_MONOTONIC, ms);
    return waitv(&w, 1, &end, CLOCK_MONOTONIC);
}
// Wait until *word >= target, futex_waitv style. 0 or -ETIMEDOUT.
static int fsync_wait_reach(volatile uint32_t* word, uint32_t target, unsigned ms)
{
    uint64_t deadline = now_ns(CLOCK_MONOTONIC) + (uint64_t)ms * 1000000ull;
    for (;;) {
        uint32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (v >= target) return 0;
        if (now_ns(CLOCK_MONOTONIC) > deadline) return -ETIMEDOUT;
        fsync_wait_value(word, v, 200);
    }
}
static void fsync_store_wake(volatile uint32_t* word, uint32_t value)
{
    __atomic_store_n(word, value, __ATOMIC_SEQ_CST);
    fwake(word, INT_MAX, 0);
}
static int fsync_open(struct fsync_map* m, const char* name, int create)
{
    memset(m, 0, sizeof(*m));
    if (create) { shm_unlink(name); m->fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0644); }
    else m->fd = shm_open(name, O_RDWR, 0644);
    if (m->fd >= 0) {
        struct stat st; fstat(m->fd, &st);
        char path[96]; snprintf(path, sizeof(path), "/dev/shm%s", name);
        struct stat byPath; int visible = stat(path, &byPath) == 0;
        printf("FSYNC_MODEL open pid=%d create=%d dev=%lx ino=%lu path-visible=%d path-ino=%lu\n", (int)getpid(), create,
               (unsigned long)st.st_dev, (unsigned long)st.st_ino, visible, visible ? (unsigned long)byPath.st_ino : 0ul);
    }
    return m->fd;
}
static int fsync_server(const char* name, int ready_fd)
{
    struct fsync_map m;
    if (fsync_open(&m, name, 1) < 0) { printf("FSYNC_MODEL server shm_open errno=%d\n", errno); return 10; }
    off_t size = FSYNC_PAGE;
    if (ftruncate(m.fd, size)) return 11;
    volatile struct fsync_ctl* ctl = (volatile struct fsync_ctl*)fsync_obj(&m, 0);
    if (!ctl) return 12;
    if (write(ready_fd, "r", 1) != 1) return 13;
    close(ready_fd);
    for (uint32_t r = 1; r <= FSYNC_ROUNDS; ++r) {
        const uint32_t idx = 16 + r * 1024 + (r * 37) % 1000; // a new page every ~4 rounds
        while ((off_t)idx * 16 >= size) { size += FSYNC_PAGE; if (ftruncate(m.fd, size)) return 14; }
        volatile uint32_t* obj = fsync_obj(&m, idx);
        if (!obj) return 15;
        __atomic_store_n(&obj[0], 0, __ATOMIC_SEQ_CST);
        ctl->idx = idx;
        fsync_store_wake(&ctl->round, r);
        if (r % 3 == 0) {
            // fsync_wake_futex: the server itself signals this round's object.
            if (fsync_wait_reach(&ctl->ready, r, 5000)) { ctl->failures++; return 16; }
            if (!__atomic_exchange_n(&obj[0], 1, __ATOMIC_SEQ_CST)) {
                long n = fwake(obj, INT_MAX, 0); if (n > 0) ctl->woken += (uint32_t)n;
            }
        }
        if (fsync_wait_reach(&ctl->done, r, 8000)) { ctl->failures++; return 17; }
    }
    return 0;
}
static int fsync_client(const char* name, int waker)
{
    struct fsync_map m;
    if (fsync_open(&m, name, 0) < 0) { printf("FSYNC_MODEL client shm_open errno=%d\n", errno); return 20; }
    volatile struct fsync_ctl* ctl = (volatile struct fsync_ctl*)fsync_obj(&m, 0);
    if (!ctl) return 21;
    for (uint32_t r = 1; r <= FSYNC_ROUNDS; ++r) {
        if (fsync_wait_reach(&ctl->round, r, 8000)) return 22;
        volatile uint32_t* obj = fsync_obj(&m, ctl->idx);
        if (!obj) return 23;
        if (!waker) {
            fsync_store_wake(&ctl->ready, r);
            long rc;
            do rc = fsync_wait_value(obj, 0, 3000);
            while (rc == -EAGAIN && !__atomic_load_n(obj, __ATOMIC_ACQUIRE));
            if (rc == -ETIMEDOUT) {
                if (__atomic_load_n(obj, __ATOMIC_ACQUIRE)) ctl->lost++; else ctl->unsignaled++;
                printf("FSYNC_MODEL round %u idx %u timeout signaled=%u\n", r, ctl->idx, *obj);
            } else if (rc != 0 && rc != -EAGAIN) ctl->failures++;
            fsync_store_wake(&ctl->done, r);
        } else if (r % 3) {
            if (fsync_wait_reach(&ctl->ready, r, 5000)) return 24;
            spin(r * 977u);
            if (!__atomic_exchange_n(obj, 1, __ATOMIC_SEQ_CST)) {
                long n = fwake(obj, INT_MAX, 0); if (n > 0) ctl->woken += (uint32_t)n;
            }
        }
    }
    return 0;
}
static const char* g_self;
static pid_t fsync_spawn(const char* role, const char* name, int fd, int isolate)
{
    // pressure-vessel style: bwrap clones into a private mount namespace in
    // which /dev/shm is reached through a bind mount of the shared directory.
    pid_t pid;
    if (!isolate) pid = fork();
    else {
        pid = (pid_t)syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
        if (pid < 0) pid = (pid_t)syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
        if (pid < 0) { printf("FSYNC_MODEL clone errno=%d\n", errno); return -1; }
    }
    if (pid) return pid;
    if (isolate) {
        if (mount(0, "/", 0, MS_REC | MS_PRIVATE, 0)) printf("FSYNC_MODEL private errno=%d\n", errno);
        if (mount("/dev/shm", "/dev/shm", 0, MS_BIND | MS_REC, 0)) { printf("FSYNC_MODEL bind errno=%d\n", errno); _exit(31); }
    }
    char fdtext[16]; snprintf(fdtext, sizeof(fdtext), "%d", fd);
    execl(g_self, g_self, role, name, fdtext, (char*)0);
    _exit(32);
}
static void test_fsync_modes(int first, int last)
{
    static const char* modes[] = {"plain", "all-bound", "server-outside", "waker-outside"};
    for (int mode = first; mode <= last; ++mode) {
        char name[64]; snprintf(name, sizeof(name), "/wine-%x%d-fsync", (unsigned)getpid(), mode);
        int p[2]; if (pipe(p)) { CHECK(0, "pipe"); return; }
        const int serverIn = mode == 1, waiterIn = mode == 1 || mode == 2 || mode == 3, wakerIn = mode == 1 || mode == 2;
        pid_t server = fsync_spawn("fsync-server", name, p[1], serverIn);
        close(p[1]);
        char c = 0; ssize_t got = read(p[0], &c, 1); close(p[0]);
        CHECK(got == 1, "%s: server not ready", modes[mode]);
        pid_t waiter = fsync_spawn("fsync-waiter", name, -1, waiterIn);
        pid_t waker = fsync_spawn("fsync-waker", name, -1, wakerIn);
        int st[3] = {0, 0, 0};
        waitpid(waiter, &st[0], 0); waitpid(waker, &st[1], 0); waitpid(server, &st[2], 0);
        // Read the outcome through a mapping of our own.
        struct fsync_map m; int fd = fsync_open(&m, name, 0);
        struct fsync_ctl out = {0};
        if (fd >= 0) { volatile struct fsync_ctl* ctl = (volatile struct fsync_ctl*)fsync_obj(&m, 0); if (ctl) out = *(struct fsync_ctl*)ctl; }
        printf("FSYNC_MODEL %s exits=%d/%d/%d open=%d rounds=%u ready=%u done=%u woken=%u lost=%u unsignaled=%u failures=%u\n",
               modes[mode], WEXITSTATUS(st[0]), WEXITSTATUS(st[1]), WEXITSTATUS(st[2]), fd >= 0, out.round, out.ready,
               out.done, out.woken, out.lost, out.unsignaled, out.failures);
        CHECK(WIFEXITED(st[0]) && !WEXITSTATUS(st[0]) && WIFEXITED(st[1]) && !WEXITSTATUS(st[1]) &&
              WIFEXITED(st[2]) && !WEXITSTATUS(st[2]), "%s: exits", modes[mode]);
        CHECK(fd >= 0 && out.done == FSYNC_ROUNDS && !out.lost && !out.unsignaled && !out.failures,
              "%s: outcome", modes[mode]);
        shm_unlink(name);
    }
}
// Wine names the shm "/wine-<ino>-fsync": the server from fstat of its
// config-dir fd (open(".") after chdir), clients from stat(path). Both must
// see one inode number, also through a bind mount, or clients open a
// different (possibly stale) segment.
static void check_prefix_inode(const char* label, const char* path, const char* alias)
{
    struct stat byPath, byFd, byAlias;
    char cwd[256]; if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    int ok = stat(path, &byPath) == 0 && chdir(path) == 0;
    int fd = ok ? open(".", O_RDONLY) : -1;
    ok = ok && fd >= 0 && fstat(fd, &byFd) == 0;
    if (fd >= 0) close(fd);
    if (chdir(cwd)) {}
    int aliased = alias && stat(alias, &byAlias) == 0;
    printf("FSYNC_MODEL prefix %s stat=%lu fstat=%lu alias=%lu\n", label, (unsigned long)byPath.st_ino,
           ok ? (unsigned long)byFd.st_ino : 0ul, aliased ? (unsigned long)byAlias.st_ino : 0ul);
    CHECK(ok && byPath.st_ino == byFd.st_ino && byPath.st_dev == byFd.st_dev, "%s: stat/fstat inode differ", label);
    if (alias) CHECK(aliased && byAlias.st_ino == byPath.st_ino, "%s: bind alias inode differs", label);
}
static void test_prefix_inode(void)
{
    mkdir("/tmp/fwv-prefix", 0755); mkdir("/tmp/fwv-alias", 0755);
    check_prefix_inode("tmp", "/tmp/fwv-prefix", 0);
    pid_t pid = (pid_t)syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
    if (pid < 0) pid = (pid_t)syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
    if (!pid) {
        mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
        failures = 0;
        if (mount("/tmp/fwv-prefix", "/tmp/fwv-alias", 0, MS_BIND, 0)) _exit(50);
        check_prefix_inode("bound", "/tmp/fwv-alias", "/tmp/fwv-prefix");
        _exit(failures ? 51 : 0);
    }
    int st = 0; if (pid > 0) waitpid(pid, &st, 0);
    CHECK(pid > 0 && WIFEXITED(st) && !WEXITSTATUS(st), "bound prefix status %d", WEXITSTATUS(st));
}
static void test_fsync_model(void)
{
    test_prefix_inode();
    test_fsync_modes(0, 3);
    // bwrap --dev/--tmpfs style: one private mount namespace whose /dev/shm is
    // a fresh container tmpfs; server, waiter and waker all live inside it.
    pid_t pid = (pid_t)syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
    if (pid < 0) pid = (pid_t)syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
    CHECK(pid >= 0, "private tmpfs clone errno=%d", errno);
    if (!pid) {
        mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
        if (mount("tmpfs", "/dev/shm", "tmpfs", MS_NODEV | MS_NOSUID, "mode=1777")) {
            printf("FSYNC_MODEL private tmpfs mount errno=%d\n", errno); _exit(40);
        }
        printf("FSYNC_MODEL private-tmpfs:\n");
        failures = 0;
        test_fsync_modes(0, 0);
        _exit(failures ? 41 : 0);
    }
    if (pid > 0) {
        int st = 0; waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "private tmpfs model status %d", WEXITSTATUS(st));
    }
    printf("FUTEX_WAITV_FSYNC_MODEL done\n");
}

// /proc/<pid>/shmframes: two processes that open one tmpfs shm by name and
// fault its untouched pages in different orders (read first / write first)
// must map the same frame per offset, and see each other's stores.
static int frame_tag(pid_t pid, unsigned offset, char* line, size_t cap)
{
    char path[64], buf[8192]; snprintf(path, sizeof(path), "/proc/%d/shmframes", (int)pid);
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char want[32]; snprintf(want, sizeof(want), "  +0x%x frame=", offset);
    char* at = strstr(buf, want); if (!at) return -1;
    char* end = strchr(at, '\n'); size_t len = end ? (size_t)(end - at) : strlen(at);
    if (len >= cap) len = cap - 1;
    memcpy(line, at, len); line[len] = 0;
    return 0;
}
static void test_shm_frames(void)
{
    char name[64]; snprintf(name, sizeof(name), "/brook-frames-%d", (int)getpid());
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0 && ftruncate(fd, 65536) == 0, "shm create");
    volatile uint32_t* mine = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int toChild[2], toParent[2];
    if (pipe(toChild) || pipe(toParent)) { CHECK(0, "pipes"); return; }
    pid_t pid = fork();
    if (!pid) {
        close(fd);
        int cfd = shm_open(name, O_RDWR, 0644);
        void* spacer = mmap(0, 3 * 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); (void)spacer;
        volatile uint32_t* theirs = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
        char c;
        uint32_t seen = theirs[0x2640 / 4];           // read-fault page 2 first
        theirs[0x46a0 / 4] = 0x5150;                  // write-fault page 4 first
        if (write(toParent[1], "1", 1) != 1) _exit(60);
        if (read(toChild[0], &c, 1) != 1) _exit(61);  // parent stored to both pages
        uint32_t later = theirs[0x2640 / 4];
        if (write(toParent[1], &seen, 4) != 4 || write(toParent[1], &later, 4) != 4) _exit(62);
        if (read(toChild[0], &c, 1) != 1) _exit(63);  // parent sampled our frames
        _exit(0);
    }
    char c;
    CHECK(read(toParent[0], &c, 1) == 1, "child ready");
    mine[0x2640 / 4] = 0xabc;                          // write-fault page 2 second
    uint32_t fromChild = mine[0x46a0 / 4];             // read-fault page 4 second
    CHECK(write(toChild[1], "2", 1) == 1, "go");
    uint32_t seen = 1, later = 0;
    CHECK(read(toParent[0], &seen, 4) == 4 && read(toParent[0], &later, 4) == 4, "child values");
    char a2[96] = "", b2[96] = "", a4[96] = "", b4[96] = "";
    frame_tag(getpid(), 0x2000, a2, sizeof(a2)); frame_tag(pid, 0x2000, b2, sizeof(b2));
    frame_tag(getpid(), 0x4000, a4, sizeof(a4)); frame_tag(pid, 0x4000, b4, sizeof(b4));
    printf("FUTEX_WAITV_SHMFRAMES parent %s | child %s\n", a2, b2);
    printf("FUTEX_WAITV_SHMFRAMES parent %s | child %s\n", a4, b4);
    CHECK(write(toChild[1], "3", 1) == 1, "done");
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "frames child %d", WEXITSTATUS(st));
    CHECK(seen == 0 && later == 0xabc && fromChild == 0x5150, "coherence seen=%u later=%#x fromChild=%#x", seen, later, fromChild);
    // Compare only the frame tag (rw/ro may legitimately differ).
    CHECK(a2[0] && b2[0] && !strncmp(a2, b2, strcspn(a2, " r") > 0 ? (size_t)(strstr(a2, " r") - a2) : strlen(a2)),
          "page 2 frames differ: %s vs %s", a2, b2);
    CHECK(a4[0] && b4[0] && !strncmp(a4, b4, strstr(a4, " r") ? (size_t)(strstr(a4, " r") - a4) : strlen(a4)),
          "page 4 frames differ: %s vs %s", a4, b4);
    munmap((void*)mine, 65536); close(fd); shm_unlink(name);
    printf("FUTEX_WAITV_SHMFRAMES done\n");
}

// ---------------------------------------------------------------- RLIMIT_NOFILE
static void test_nofile(void)
{
    struct rlimit r;
    CHECK(getrlimit(RLIMIT_NOFILE, &r) == 0, "getrlimit");
    printf("FUTEX_WAITV_NOFILE initial soft=%llu hard=%llu\n", (unsigned long long)r.rlim_cur, (unsigned long long)r.rlim_max);
    CHECK(r.rlim_max >= 524288, "hard %llu", (unsigned long long)r.rlim_max);
    pid_t pid = fork();
    if (!pid) {
        struct rlimit x; getrlimit(RLIMIT_NOFILE, &x);
        x.rlim_cur = x.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &x)) _exit(1);
        struct rlimit y; getrlimit(RLIMIT_NOFILE, &y);
        if (y.rlim_cur != x.rlim_max) _exit(2);
        struct rlimit64 { uint64_t cur, max; } old, lower = {1024, 4096}, higher = {1024, 8192}, inverted = {8192, 4096};
        if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &lower, &old) || old.cur != x.rlim_max) _exit(3);
        if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &inverted, 0) != -1 || errno != EINVAL) _exit(4);
        // Raising the hard limit needs CAP_SYS_RESOURCE; uid 0 test runs have it.
        long raised = syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &higher, 0);
        if (geteuid() != 0 && (raised != -1 || errno != EPERM)) _exit(5);
        // Exec/fork inheritance: a grandchild sees the same limits.
        pid_t g = fork();
        if (!g) { struct rlimit z; getrlimit(RLIMIT_NOFILE, &z); _exit(z.rlim_cur == 1024 && z.rlim_max >= 4096 ? 0 : 6); }
        int st = 0; waitpid(g, &st, 0);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 7);
    }
    int status = 0; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "nofile child status %d", WEXITSTATUS(status));
    struct rlimit after; getrlimit(RLIMIT_NOFILE, &after);
    CHECK(after.rlim_cur == r.rlim_cur && after.rlim_max == r.rlim_max, "child changed parent limits");
    printf("FUTEX_WAITV_NOFILE done\n");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    g_self = argv[0];
    if (argc == 4 && !strcmp(argv[1], "fsync-server")) return fsync_server(argv[2], atoi(argv[3]));
    if (argc == 4 && !strcmp(argv[1], "fsync-waiter")) return fsync_client(argv[2], 0);
    if (argc == 4 && !strcmp(argv[1], "fsync-waker")) return fsync_client(argv[2], 1);
    if (argc == 2 && !strcmp(argv[1], "fsync")) { test_fsync_model(); printf(failures ? "FUTEX_WAITV_FAIL\n" : "FUTEX_WAITV_OK\n"); return failures != 0; }
    uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 10) : 100000;
    test_arguments();
    test_mismatch_timeout();
    test_index();
    test_multi_waiters();
    test_proc_futex();
    test_signal();
    test_cross_process();
    test_killed_waiters();
    test_shm_frames();
    test_fsync_model();
    test_nofile();
    test_stress(iterations);
    if (failures) printf("FUTEX_WAITV_FAIL failures=%d\n", failures);
    else printf("FUTEX_WAITV_OK\n");
    fflush(stdout);
    return failures ? 1 : 0;
}

#define _GNU_SOURCE
/* Controlled AS-validation/scheduler workload. Uses actual clock syscalls,
 * separate pthread stacks and reserved VMAs; optional libc clock path.
 * No network or disk traffic. Default mode forces the syscall.
 * READY/RESULT lead/tail intervals let an external observer bound counters. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_barrier_t start;
static unsigned iterations;
static int use_libc;
static void *worker(void *unused) {
    (void)unused;
    int result = pthread_barrier_wait(&start);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    struct timespec stamp;
    for (unsigned i = 0; i < iterations; ++i) {
        assert((use_libc ? clock_gettime(CLOCK_MONOTONIC, &stamp)
                         : syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &stamp)) == 0);
        assert(stamp.tv_sec >= 0 && stamp.tv_nsec >= 0 && stamp.tv_nsec < 1000000000);
    }
    return NULL;
}
static uint64_t now(void) {
    struct timespec stamp;
    assert(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &stamp) == 0);
    return (uint64_t)stamp.tv_sec * 1000000000 + stamp.tv_nsec;
}
int main(int argc, char **argv) {
    assert(argc == 4 || argc == 5);
    if (argc == 5) {
        assert(!strcmp(argv[4], "libc") || !strcmp(argv[4], "syscall"));
        use_libc = !strcmp(argv[4], "libc");
    }
    unsigned threads = (unsigned)strtoul(argv[1], NULL, 10);
    iterations = (unsigned)strtoul(argv[2], NULL, 10);
    unsigned lead = (unsigned)strtoul(argv[3], NULL, 10);
    assert(threads >= 1 && threads <= 128 && iterations >= 1 && iterations <= 100000 && lead <= 10);
    void *reservations[512];
    for (unsigned i = 0; i < 512; ++i) {
        reservations[i] = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(reservations[i] != MAP_FAILED);
    }
    assert(pthread_barrier_init(&start, NULL, threads + 1) == 0);
    pthread_t handles[128];
    for (unsigned i = 0; i < threads; ++i) assert(pthread_create(&handles[i], NULL, worker, NULL) == 0);
    puts("READY"); fflush(stdout);
    sleep(lead);
    uint64_t began = now();
    int result = pthread_barrier_wait(&start);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i = 0; i < threads; ++i) assert(pthread_join(handles[i], NULL) == 0);
    uint64_t elapsed = now() - began;
    printf("RESULT {\"threads\":%u,\"iterations_per_thread\":%u,\"clock_calls\":%u,"
           "\"guest_duration_ns\":%llu,\"reserved_ranges\":512,"
           "\"clock_mode\":\"%s\",\"vdso_available\":%s}\n",
           threads, iterations, threads * iterations, (unsigned long long)elapsed,
           use_libc ? "libc" : "syscall", getauxval(AT_SYSINFO_EHDR) ? "true" : "false");
    fflush(stdout);
    sleep(lead); /* Allow observer to stop counters before reservation teardown. */
    assert(pthread_barrier_destroy(&start) == 0);
    for (unsigned i = 0; i < 512; ++i) assert(munmap(reservations[i], 4096) == 0);
    puts("CLOCK_THREADS_PASS");
    return 0;
}

#define _GNU_SOURCE
/* Bounded CPU saturation plus 16-ms deadline sleepers. No devices or network.
 * Service counters measure this fixed workload, not general CPU utilization.
 * The host observer must independently bound duration and record host load. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS 64
#define SLEEPERS 4
#define MAX_SAMPLES 2048
#define PERIOD_NS 16000000ULL
struct result { uint64_t batches, signature, first, last, missed, early; unsigned samples; uint64_t late[MAX_SAMPLES]; };
static struct result results[MAX_WORKERS + SLEEPERS];
static pthread_barrier_t barrier;
static uint64_t start_ns, end_ns;
static unsigned worker_count;
static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static void until(uint64_t target) {
    struct timespec ts = {(time_t)(target / 1000000000ULL), (long)(target % 1000000000ULL)};
    while (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL)) {
        if (errno != EINTR) { perror("clock_nanosleep"); abort(); }
    }
}
static void *run(void *argument) {
    unsigned index = (unsigned)(uintptr_t)argument;
    struct result *r = &results[index];
    int rc = pthread_barrier_wait(&barrier);
    if (rc && rc != PTHREAD_BARRIER_SERIAL_THREAD) abort();
    until(start_ns);
    r->first = now_ns();
    if (index < worker_count) {
        uint64_t value = index + 1;
        while (now_ns() < end_ns) {
            for (unsigned i = 0; i < 32768; ++i)
                value = value * 6364136223846793005ULL + 1442695040888963407ULL;
            ++r->batches;
        }
        r->signature = value;
    } else {
        uint64_t target = start_ns + PERIOD_NS;
        while (target < end_ns && r->samples < MAX_SAMPLES) {
            until(target);
            uint64_t observed = now_ns();
            if (observed < target) ++r->early;
            r->late[r->samples++] = observed > target ? observed-target : 0;
            target += PERIOD_NS;
            // Skip missed periods instead of generating a catch-up busy loop.
            if (target < observed) {
                uint64_t skip = (observed-target) / PERIOD_NS + 1;
                r->missed += skip;
                target += skip * PERIOD_NS;
            }
        }
    }
    r->last = now_ns();
    return NULL;
}
static int compare(const void *a, const void *b) {
    uint64_t first = *(const uint64_t *)a, second = *(const uint64_t *)b;
    return (first > second) - (first < second);
}
static uint64_t percentile(struct result *r, unsigned pct) {
    if (!r->samples) return 0;
    unsigned rank = (r->samples * pct + 99) / 100;
    return r->late[rank-1];
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr,"usage: scheduler_mix CPU_WORKERS SECONDS\n"); return 64; }
    char *tail;
    unsigned long workers = strtoul(argv[1], &tail, 10);
    if (*tail || !workers || workers > MAX_WORKERS) return 64;
    unsigned long seconds = strtoul(argv[2], &tail, 10);
    if (*tail || !seconds || seconds > 30) return 64;
    worker_count = (unsigned)workers;
    unsigned total = worker_count + SLEEPERS;
    pthread_t threads[MAX_WORKERS + SLEEPERS];
    if (pthread_barrier_init(&barrier, NULL, total + 1)) return 1;
    for (unsigned i = 0; i < total; ++i) {
        int error = pthread_create(&threads[i], NULL, run, (void *)(uintptr_t)i);
        if (error) { fprintf(stderr,"pthread_create: %s\n",strerror(error)); return 1; }
    }
    start_ns = now_ns() + 1000000000ULL;
    end_ns = start_ns + seconds * 1000000000ULL;
    printf("READY workers=%u sleepers=%u seconds=%lu\n", worker_count, SLEEPERS, seconds);
    fflush(stdout);
    int rc = pthread_barrier_wait(&barrier);
    if (rc && rc != PTHREAD_BARRIER_SERIAL_THREAD) abort();
    for (unsigned i = 0; i < total; ++i) if (pthread_join(threads[i], NULL)) abort();
    printf("RESULT {\"workers\":%u,\"sleepers\":%u,\"requested_ns\":%llu,\"start_ns\":%llu,\"end_ns\":%llu,\"cpu\":[", worker_count, SLEEPERS,
           (unsigned long long)(end_ns-start_ns), (unsigned long long)start_ns, (unsigned long long)end_ns);
    for (unsigned i = 0; i < total; ++i) {
        struct result *r = &results[i];
        if (i == worker_count) printf("],\"periodic\":[");
        if (i != 0 && i != worker_count) putchar(',');
        if (i < worker_count)
            printf("{\"id\":%u,\"batches\":%llu,\"signature\":%llu,\"first_ns\":%llu,\"last_ns\":%llu}", i,
                   (unsigned long long)r->batches, (unsigned long long)r->signature,
                   (unsigned long long)r->first, (unsigned long long)r->last);
        else {
            qsort(r->late, r->samples, sizeof(r->late[0]), compare);
            printf("{\"id\":%u,\"samples\":%u,\"missed_periods\":%llu,\"early\":%llu,\"p50_ns\":%llu,\"p90_ns\":%llu,\"p95_ns\":%llu,\"max_ns\":%llu}", i,
                   r->samples, (unsigned long long)r->missed, (unsigned long long)r->early,
                   (unsigned long long)percentile(r,50), (unsigned long long)percentile(r,90),
                   (unsigned long long)percentile(r,95), (unsigned long long)percentile(r,100));
        }
    }
    puts("]}");
    pthread_barrier_destroy(&barrier);
    return 0;
}

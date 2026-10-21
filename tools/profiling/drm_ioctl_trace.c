// Optional LD_PRELOAD diagnostic: bounded, allocation-free DRM call aggregates.
// SDL probe hooks distinguish the swap thread's inside/outside-swap calls.
// Elapsed time includes scheduling; concurrent threads must not be summed as
// frame wall time. No output is written until normal process teardown.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { Slots = 128, Buckets = 64 };
struct Aggregate {
    uint64_t key, calls, sum_ns, max_ns, errors;
    uint64_t histogram[Buckets];
};
static struct Aggregate entries[Slots];
static uint64_t invalid_clock, overflow, saturated, first_ns, last_ns;
static unsigned closing, active;
static int (*next_ioctl)(int, unsigned long, void *);
static const char *output;
static unsigned skip_swaps = 50;
static __thread unsigned swaps, phase, swap_depth;
static __thread long tid;
struct Lookup { void *symbol; };

// Mesa's renderer may load libdrm in an RTLD_LOCAL group. RTLD_NEXT from a
// preload then cannot see it. Obtain the already-loaded library explicitly;
// keep the reference so a cached pointer cannot outlive its code mapping.
static int find_local_drm(struct dl_phdr_info *info, size_t size, void *value) {
    (void)size;
    const char *name = strrchr(info->dlpi_name, '/');
    name = name ? name+1 : info->dlpi_name;
    if (strncmp(name, "libdrm.so.", 10)) return 0;
    void *handle = dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return 0;
    void *symbol = dlsym(handle, "drmIoctl");
    if (!symbol) { dlclose(handle); return 0; }
    ((struct Lookup *)value)->symbol = symbol;
    return 1;
}

static void maximum(uint64_t *p, uint64_t n) {
    uint64_t old = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (old < n && !__atomic_compare_exchange_n(p, &old, n, 1,
                                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void add(uint64_t *p, uint64_t n) {
    uint64_t old = __atomic_load_n(p, __ATOMIC_RELAXED);
    for (;;) {
        if (UINT64_MAX - old < n) {
            __atomic_store_n(&saturated, 1, __ATOMIC_RELAXED);
            return;
        }
        if (__atomic_compare_exchange_n(p, &old, old+n, 1,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    }
}
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 ||
        (uint64_t)t.tv_sec > UINT64_MAX/1000000000ULL - 1 ||
        t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) {
        add(&invalid_clock, 1);
        return 0;
    }
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
__attribute__((constructor)) static void init(void) {
    output = getenv("BROOK_DRM_TRACE");
    const char *value = getenv("BROOK_DRM_TRACE_SKIP_SWAPS");
    if (value) {
        char *end;
        errno = 0;
        unsigned long n = strtoul(value, &end, 10);
        if (errno || !*value || *end || n > 8192) {
            fputs("drm_ioctl_trace: invalid skip count\n", stderr);
            _exit(127);
        }
        skip_swaps = (unsigned)n;
    }
}
// Called only by the optional SDL probe, on its swap thread. Other threads
// remain unscoped (phase0), rather than being mislabeled as between swaps.
void brook_drm_trace_swap_enter(void) {
    if (swap_depth++) { add(&overflow, 1); return; }
    if (swaps != ~0u) ++swaps;
    phase = swaps > skip_swaps ? 2 : 0;
}
void brook_drm_trace_swap_leave(void) {
    if (!swap_depth) { add(&overflow, 1); return; }
    if (--swap_depth) return;
    phase = swaps > skip_swaps ? 1 : 0;
}

int drmIoctl(int fd, unsigned long request, void *arg) {
    int prior_errno = errno;
    int (*call)(int, unsigned long, void *) = __atomic_load_n(&next_ioctl, __ATOMIC_ACQUIRE);
    if (!call) {
        // libdrm may be dlopened after preload constructors have run.
        call = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "drmIoctl");
        if (!call) {
            struct Lookup lookup = { 0 };
            dl_iterate_phdr(find_local_drm, &lookup);
            call = (int (*)(int, unsigned long, void *))lookup.symbol;
        }
        if (!call) { fputs("drm_ioctl_trace: missing next drmIoctl\n", stderr); _exit(127); }
        __atomic_store_n(&next_ioctl, call, __ATOMIC_RELEASE);
    }
    if (!output || !*output) { errno = prior_errno; return call(fd, request, arg); }
    // A single total order makes close/admission linearizable even when a
    // call is entering while another thread starts process teardown.
    if (__atomic_load_n(&closing, __ATOMIC_SEQ_CST)) { errno = prior_errno; return call(fd, request, arg); }
    __atomic_fetch_add(&active, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&closing, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&active, 1, __ATOMIC_SEQ_CST);
        errno = prior_errno;
        return call(fd, request, arg);
    }
    if (!tid) tid = syscall(SYS_gettid);
    unsigned call_phase = phase;
    uint64_t begin = now_ns();
    errno = prior_errno;
    int result = call(fd, request, arg);
    int saved_errno = errno;
    uint64_t end = now_ns();
    if (!begin || !end || end < begin) { add(&invalid_clock, 1); goto done; }
    uint64_t old = __atomic_load_n(&first_ns, __ATOMIC_RELAXED);
    while ((!old || begin < old) && !__atomic_compare_exchange_n(&first_ns, &old, begin, 1,
                                                               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    maximum(&last_ns, end);
    if (request > UINT32_MAX || tid <= 0 || tid > 0x3fffffff) { add(&overflow, 1); goto done; }
    uint64_t key = ((uint64_t)tid << 34) | ((uint64_t)request << 2) | call_phase;
    for (unsigned i = 0; i < Slots; ++i) {
        struct Aggregate *a = &entries[i];
        uint64_t found = __atomic_load_n(&a->key, __ATOMIC_ACQUIRE);
        if (!found) __atomic_compare_exchange_n(&a->key, &found, key, 0,
                                               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        if (found != key && __atomic_load_n(&a->key, __ATOMIC_ACQUIRE) != key) continue;
        uint64_t ns = end-begin;
        unsigned bucket = ns ? 63u-(unsigned)__builtin_clzll(ns) : 0;
        add(&a->calls, 1); add(&a->sum_ns, ns); maximum(&a->max_ns, ns);
        if (result < 0) add(&a->errors, 1);
        add(&a->histogram[bucket], 1);
        goto done;
    }
    add(&overflow, 1);
done:
    __atomic_fetch_sub(&active, 1, __ATOMIC_SEQ_CST);
    errno = saved_errno;
    return result;
}

__attribute__((destructor)) static void finish(void) {
    if (!output || !*output) return;
    __atomic_store_n(&closing, 1, __ATOMIC_SEQ_CST);
    unsigned incomplete = __atomic_load_n(&active, __ATOMIC_SEQ_CST);
    if (!incomplete && !__atomic_load_n(&first_ns, __ATOMIC_RELAXED)) return;
    FILE *f = fopen(output, "wx");
    if (!f) { perror("drm_ioctl_trace output"); return; }
    #define READ(v) __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
    fprintf(f, "# brook_drm_trace_v1 invalid_clock=%llu overflow=%llu saturated=%llu incomplete=%u first_ns=%llu last_ns=%llu skip_swaps=%u\n",
            (unsigned long long)READ(invalid_clock), (unsigned long long)READ(overflow),
            (unsigned long long)READ(saturated), incomplete, (unsigned long long)READ(first_ns),
            (unsigned long long)READ(last_ns), skip_swaps);
    fputs("tid,phase,request,calls,sum_ns,max_ns,errors,histogram_log2\n", f);
    for (unsigned i = 0; i < Slots; ++i) {
        struct Aggregate *a = &entries[i];
        uint64_t key = READ(a->key);
        if (!key) continue;
        fprintf(f, "%llu,%u,0x%08x,%llu,%llu,%llu,%llu,",
                (unsigned long long)(key >> 34), (unsigned)(key & 3),
                (unsigned)((key >> 2) & UINT32_MAX),
                (unsigned long long)READ(a->calls), (unsigned long long)READ(a->sum_ns),
                (unsigned long long)READ(a->max_ns), (unsigned long long)READ(a->errors));
        for (unsigned j = 0; j < Buckets; ++j)
            fprintf(f, "%s%llu", j ? ";" : "", (unsigned long long)READ(a->histogram[j]));
        fputc('\n', f);
    }
    if (fclose(f)) perror("drm_ioctl_trace flush");
    #undef READ
}

// Optional LD_PRELOAD probe. Records SDL application swap cadence, not display
// scanout. No per-frame allocation or I/O; reject overflow/incomplete captures.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { Capacity = 8192 };
struct Entry { uint64_t begin, end; long tid; unsigned ready; };
static struct Entry entries[Capacity];
static unsigned count, invalid_clock;
static void (*real_swap)(void *);

static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 ||
        t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) {
        __atomic_fetch_add(&invalid_clock, 1, __ATOMIC_RELAXED);
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

__attribute__((constructor)) static void init(void) {
    real_swap = (void (*)(void *))dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
}

void SDL_GL_SwapWindow(void *window) {
    if (!real_swap) {
        fputs("sdl_swap_trace: missing next SDL_GL_SwapWindow\n", stderr);
        _exit(127);
    }
    const unsigned slot = __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED);
    if (slot >= Capacity) { real_swap(window); return; }
    struct Entry *e = &entries[slot];
    e->tid = syscall(SYS_gettid);
    e->begin = now_ns();
    real_swap(window);
    e->end = now_ns();
    __atomic_store_n(&e->ready, 1, __ATOMIC_RELEASE);
}

__attribute__((destructor)) static void finish(void) {
    const unsigned total = __atomic_load_n(&count, __ATOMIC_ACQUIRE);
    const char *path = getenv("BROOK_SWAP_TRACE");
    if (!total || !path || !*path) return;
    FILE *f = fopen(path, "wx");
    if (!f) { perror("sdl_swap_trace output"); return; }
    unsigned incomplete = 0;
    const unsigned kept = total < Capacity ? total : Capacity;
    for (unsigned i = 0; i < kept; ++i)
        if (!__atomic_load_n(&entries[i].ready, __ATOMIC_ACQUIRE)) ++incomplete;
    fprintf(f, "# brook_swap_trace_v1 count=%u dropped=%u incomplete=%u invalid_clock=%u\n",
            kept, total - kept, incomplete, invalid_clock);
    fputs("index,tid,begin_ns,end_ns\n", f);
    for (unsigned i = 0; i < kept; ++i) {
        struct Entry *e = &entries[i];
        if (__atomic_load_n(&e->ready, __ATOMIC_ACQUIRE))
            fprintf(f, "%u,%ld,%llu,%llu\n", i, e->tid,
                    (unsigned long long)e->begin, (unsigned long long)e->end);
    }
    if (fclose(f)) perror("sdl_swap_trace flush");
}

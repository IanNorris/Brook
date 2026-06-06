/* schedstress.c — SMP scheduler thread-group churn stress test (BRO-173).
 *
 * Reproduces the pick->context-switch use-after-free: a thread that the
 * timer tick has just selected (dequeued from the ready queue, runningOnCpu
 * still -1, g_readyLock released) but not yet switched to can be terminated
 * and reaped by a concurrent exit_group on another CPU, after which
 * context_switch loads its freed+poisoned kernel stack and jumps to garbage
 * ("execute from unmapped page", R10=0xcc.. poison).
 *
 * Strategy: fork many short-lived child processes; each child spawns several
 * CPU-spinning threads and then immediately exit_group()s WITHOUT joining
 * them, so the kernel must tear down (and reap) sibling threads that are
 * actively Ready/Running across all CPUs.  High fork+thread+exit_group churn
 * across the SMP set maximises the chance of hitting the narrow window.
 *
 * Pure CPU-bound threads keep the victims schedulable (Ready) so PickNext
 * frequently selects them exactly when the group is being killed.
 *
 * Build: part of src/apps (musl static).  Run: schedstress [rounds] [procs] [threads]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/syscall.h>

/* Monotonic milliseconds, or 0 if the clock is unavailable. */
static unsigned long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}

/* Defaults chosen for HIGH-FREQUENCY exit_group churn: short-lived children
 * that spawn threads and tear the group down almost immediately, so victim
 * threads are Ready/just-scheduled when exit_group hits — the BRO-173 window.
 * Bounded per-thread spins (no infinite loops) prevent runnable threads from
 * accumulating and permanently saturating the CPUs (which throttles turnover).
 */
#define DEFAULT_ROUNDS        8000  /* number of child processes spawned */
#define DEFAULT_CONCURRENT    2     /* children alive at once (low oversub) */
#define DEFAULT_THREADS       12    /* threads spawned per child (> CPUs) */

static volatile unsigned long g_sink; /* defeat dead-code elimination */

static void *spinner(void *arg)
{
    (void)arg;
    /* Bounded CPU spin: long enough to stay Ready across a few scheduling
     * decisions (so PickNext selects us), short enough that an escaping
     * thread terminates instead of saturating a CPU forever. */
    unsigned long acc = (unsigned long)(uintptr_t)arg;
    for (int r = 0; r < 300; r++) {
        for (int i = 0; i < 1000; i++)
            acc += (unsigned long)i * 2654435761u + acc;
        g_sink = acc;
    }
    return (void *)acc;
}

/* Child body: spawn threads and IMMEDIATELY exit_group the whole group
 * without joining — victim threads are freshly Ready (often not yet run)
 * when the group is torn down, hitting the pick->switch window. */
static void child_body(int nthreads)
{
    pthread_t t[64];
    if (nthreads > 64) nthreads = 64;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&t[i], NULL, spinner, (void *)(uintptr_t)i) != 0)
            break; /* partial set still triggers the race */
    }
    /* No leader burst, no join: tear the group down right away so siblings
     * are in the Ready/just-picked state when exit_group fires. */
    syscall(SYS_exit_group, 0);
    _exit(0); /* not reached */
}

int main(int argc, char **argv)
{
    int rounds     = (argc > 1) ? atoi(argv[1]) : DEFAULT_ROUNDS;
    int concurrent = (argc > 2) ? atoi(argv[2]) : DEFAULT_CONCURRENT;
    int nthreads   = (argc > 3) ? atoi(argv[3]) : DEFAULT_THREADS;
    if (rounds <= 0)     rounds = DEFAULT_ROUNDS;
    if (concurrent <= 0) concurrent = DEFAULT_CONCURRENT;
    if (nthreads <= 0)   nthreads = DEFAULT_THREADS;

    /* Line-buffer stdout so progress/summary appear immediately even when
     * stdout is a pipe/serial sink rather than a TTY. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== schedstress (BRO-173): rounds=%d concurrent=%d threads/proc=%d ===\n",
           rounds, concurrent, nthreads);
    fflush(stdout);

    int live = 0;
    int spawned = 0, reaped = 0;
    unsigned long t0 = now_ms();

    for (int r = 0; r < rounds; r++) {
        pid_t pid = fork();
        if (pid < 0) {
            /* Out of process slots — drain and retry. */
            int st; pid_t w = waitpid(-1, &st, 0);
            if (w > 0) { live--; reaped++; }
            r--;
            continue;
        }
        if (pid == 0) {
            child_body(nthreads);
            _exit(0); /* not reached */
        }
        spawned++;
        live++;

        /* Keep `concurrent` children alive so multiple groups are being
         * created and torn down simultaneously across the CPUs. */
        while (live >= concurrent) {
            int st; pid_t w = waitpid(-1, &st, 0);
            if (w > 0) { live--; reaped++; }
            else break;
        }

        if ((r & 63) == 0) {
            printf("  progress: spawned=%d reaped=%d live=%d\n",
                   spawned, reaped, live);
            fflush(stdout);
        }
    }

    /* Drain remaining children. */
    while (live > 0) {
        int st; pid_t w = waitpid(-1, &st, 0);
        if (w > 0) { live--; reaped++; }
        else break;
    }

    unsigned long t1 = now_ms();
    unsigned long elapsed_ms = (t1 >= t0) ? (t1 - t0) : 0;

    /* If we reached here at all, the kernel survived the churn (no scheduler
     * panic/UAF/livelock would have let us return).  The remaining health
     * check is that every spawned child was reaped — a leak or stuck zombie
     * shows up as reaped < spawned. */
    int leaked = spawned - reaped;
    int pass = (live == 0 && leaked == 0);

    /* threads created total = one spinner set per child. */
    unsigned long total_threads = (unsigned long)spawned * (unsigned long)nthreads;

    printf("\n");
    printf("================ schedstress TEST COMPLETE ================\n");
    printf("  result          : %s\n", pass ? "PASS" : "FAIL");
    printf("  children spawned : %d\n", spawned);
    printf("  children reaped  : %d\n", reaped);
    printf("  leaked/stuck     : %d\n", leaked);
    printf("  threads created  : %lu  (%d per child)\n", total_threads, nthreads);
    printf("  elapsed          : %lu ms\n", elapsed_ms);
    if (elapsed_ms > 0) {
        unsigned long cps = (unsigned long)spawned * 1000UL / elapsed_ms;
        unsigned long tps = total_threads * 1000UL / elapsed_ms;
        printf("  throughput       : %lu children/s, %lu threads/s\n", cps, tps);
    }
    printf("  scheduler        : no panic / UAF / livelock\n");
    printf("==========================================================\n");
    fflush(stdout);
    return pass ? 0 : 1;
}

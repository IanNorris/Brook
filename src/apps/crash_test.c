/* crash_test.c — Deliberate crash generator for testing Brook's panic infrastructure.
 *
 * Build: musl-gcc -static -o crash_test crash_test.c
 * Usage: crash_test <mode>
 *   null       — dereference NULL pointer (SIGSEGV / #PF)
 *   groupfault — spawn blocked sibling threads, then leader #PFs (BRO-176)
 *   divzero    — integer divide by zero (#DE)
 *   stackoverflow — infinite recursion (stack overflow → #PF)
 *   gpf        — execute privileged instruction from userspace (#GP)
 *   ud         — execute undefined opcode (#UD)
 *   wild       — jump to wild address (0xDEADBEEF)
 *   writekernel — write to kernel address space (#PF / #GP)
 *   readkernel  — read from kernel address space (#PF)
 *   int3       — breakpoint trap (#BP)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile int g_sink;

/* BRO-176 repro: a fatal user fault in a thread-group LEADER that still has
 * live sibling threads must be group-fatal (tear the whole group down), not
 * leave the leader a permanent zombie waiting on siblings that never exit.
 *
 * We fork a child that becomes a thread-group leader: it spawns N siblings that
 * block forever in a futex wait (pthread_mutex_lock on a mutex the child holds),
 * waits until they are all blocked, then deliberately #PFs. The PARENT waitpid()s
 * the child. Expected with the fix: the child's whole group is torn down, the
 * leader becomes a reapable zombie, waitpid() returns, and we print the marker
 * below. Without the fix: asLiveThreads never drains to 0, the child-leader is
 * never reapable, the parent's waitpid() hangs forever (reap-stall) and the
 * marker never appears. The marker line in the serial log is the PASS signal. */
#define GROUPFAULT_NTHREADS 4
static pthread_mutex_t g_blockMutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_blockedCount;

__attribute__((noinline))
static void *groupfault_sibling(void *arg) {
    (void)arg;
    atomic_fetch_add(&g_blockedCount, 1);
    /* Blocks forever: the leader holds g_blockMutex and never releases it. */
    pthread_mutex_lock(&g_blockMutex);
    pthread_mutex_unlock(&g_blockMutex);
    return NULL;
}

__attribute__((noinline))
static void crash_groupfault(void) {
    pid_t child = fork();
    if (child == 0) {
        /* CHILD: thread-group leader that will fault with live siblings. */
        pthread_mutex_lock(&g_blockMutex);   /* held forever; siblings block */
        pthread_t th[GROUPFAULT_NTHREADS];
        for (int i = 0; i < GROUPFAULT_NTHREADS; i++)
            pthread_create(&th[i], NULL, groupfault_sibling, NULL);

        /* Wait until every sibling has reached its blocking mutex_lock. */
        while (atomic_load(&g_blockedCount) < GROUPFAULT_NTHREADS) { /* spin */ }
        /* Let the last sibling actually enter the futex wait (it bumped the
         * counter immediately before the lock call). */
        for (volatile uint64_t i = 0; i < 50000000ULL; i++) { /* settle */ }

        printf("crash_groupfault: child %d, %d siblings blocked, leader faulting\n",
               (int)getpid(), GROUPFAULT_NTHREADS);
        volatile int *p = (volatile int *)0;   /* leader #PF with live siblings */
        g_sink = *p;
        _exit(99);   /* never reached */
    }

    /* PARENT: this returns only if the faulting child-group gets reaped. */
    int status = 0;
    pid_t r = waitpid(child, &status, 0);

    /* Bulletproof reporting. qemu block-buffers -serial output to a file, so a
     * single trailing line can be lost; emit a durable disk file AND a long
     * keep-alive stream so the marker is guaranteed to flush to the host. If the
     * reap-stall bug is present, waitpid() never returns and NONE of this runs. */
    printf("BRO176-PARENT-REAPED-CHILD child=%d ret=%d status=0x%x\n",
           (int)child, (int)r, status);
    FILE *f = fopen("/data/bro176_result.txt", "w");
    if (f) {
        fprintf(f, "REAPED child=%d ret=%d status=0x%x\n", (int)child, (int)r, status);
        fclose(f);
    }
    for (int i = 0; i < 200; i++) {
        printf("BRO176-ALIVE-%d reaped-child=%d\n", i, (int)child);
        for (volatile uint64_t k = 0; k < 8000000ULL; k++) { /* pace */ }
    }
}

__attribute__((noinline))
static void crash_null_deref(void) {
    volatile int *p = (volatile int *)0;
    g_sink = *p;
}

__attribute__((noinline))
static void crash_div_zero(void) {
    volatile int a = 1;
    volatile int b = 0;
    g_sink = a / b;
}

/* Recursive function for stack overflow */
__attribute__((noinline))
static void crash_recurse(volatile int depth) {
    volatile char buf[4096]; /* eat stack fast */
    buf[0] = (char)depth;
    g_sink = buf[0];
    crash_recurse(depth + 1);
}

__attribute__((noinline))
static void crash_stackoverflow(void) {
    crash_recurse(0);
}

__attribute__((noinline))
static void crash_gpf(void) {
    /* HLT is ring-0 only — triggers #GP from userspace */
    __asm__ volatile("hlt");
}

__attribute__((noinline))
static void crash_ud(void) {
    /* UD2 — guaranteed undefined opcode */
    __asm__ volatile("ud2");
}

__attribute__((noinline))
static void crash_wild_jump(void) {
    void (*fn)(void) = (void (*)(void))0xDEADBEEF;
    fn();
}

__attribute__((noinline))
static void crash_write_kernel(void) {
    /* Write to a typical kernel address */
    volatile uint64_t *p = (volatile uint64_t *)0xFFFFFFFF80000000ULL;
    *p = 0x4242424242424242ULL;
}

__attribute__((noinline))
static void crash_read_kernel(void) {
    volatile uint64_t *p = (volatile uint64_t *)0xFFFFFFFF80000000ULL;
    g_sink = (int)*p;
}

__attribute__((noinline))
static void crash_int3(void) {
    __asm__ volatile("int3");
}

struct crash_mode {
    const char *name;
    void (*fn)(void);
    const char *desc;
};

static const struct crash_mode modes[] = {
    { "null",         crash_null_deref,   "NULL pointer dereference (#PF)" },
    { "groupfault",   crash_groupfault,   "Leader #PF with live sibling threads (BRO-176)" },
    { "divzero",      crash_div_zero,     "Integer divide by zero (#DE)" },
    { "stackoverflow",crash_stackoverflow,"Stack overflow via recursion (#PF)" },
    { "gpf",          crash_gpf,          "Privileged instruction from user (#GP)" },
    { "ud",           crash_ud,           "Undefined opcode (#UD)" },
    { "wild",         crash_wild_jump,    "Jump to wild address 0xDEADBEEF" },
    { "writekernel",  crash_write_kernel, "Write to kernel address (#PF/#GP)" },
    { "readkernel",   crash_read_kernel,  "Read from kernel address (#PF)" },
    { "int3",         crash_int3,         "Breakpoint trap (#BP)" },
    { NULL, NULL, NULL }
};

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("crash_test — deliberate crash generator\n\nUsage: crash_test <mode>\n\nModes:\n");
        for (int i = 0; modes[i].name; i++)
            printf("  %-16s %s\n", modes[i].name, modes[i].desc);
        return 1;
    }

    for (int i = 0; modes[i].name; i++) {
        if (strcmp(argv[1], modes[i].name) == 0) {
            printf("crash_test: triggering '%s' — %s\n", modes[i].name, modes[i].desc);
            modes[i].fn();
            printf("crash_test: ERROR — crash did not occur?!\n");
            return 2;
        }
    }

    printf("crash_test: unknown mode '%s'\n", argv[1]);
    return 1;
}

#define _GNU_SOURCE
/* libc discovery, cross-thread syscall bracketing and mapping lifetimes.
 * Build both static musl and dynamic glibc variants for Brook acceptance. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t ns(struct timespec value) {
    assert(value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static void *worker(void *unused) {
    (void)unused;
    uint64_t last = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        struct timespec before, value, after;
        assert(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &before) == 0);
        assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
        assert(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &after) == 0);
        assert(ns(before) <= ns(value) && ns(value) <= ns(after) && ns(value) >= last);
        last = ns(value);
        if (!(i % 100)) sched_yield();
    }
    return NULL;
}
static void clean_child(pid_t child) {
    int status;
    assert(child > 0 && waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void protected_fault(uintptr_t address, int execute) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        if (execute) ((void (*)(void))address)();
        else {
            volatile unsigned char *byte = (void *)address;
            *byte = *byte; /* Same byte, so a bad PTE cannot corrupt shared code. */
        }
        _exit(99);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}
int main(int argc, char **argv) {
    assert(argc >= 1);
    uintptr_t base = getauxval(AT_SYSINFO_EHDR);
    assert(base && !(base & 4095) && !memcmp((void *)base, "\177ELF", 4));
    if (argc == 2 && !strcmp(argv[1], "--exec")) {
        worker(NULL);
        puts("VDSO_EXEC_PASS");
        return 0;
    }
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
    for (unsigned i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(mprotect((void *)base, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    assert(mprotect((void *)(base+4096), 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    assert(mprotect((void *)(base+4096), 4096, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
    protected_fault(base, 0);
    protected_fault(base+8191, 0); /* Clock padding, outside host-updated fields. */
    protected_fault(base+4096, 1);
    for (unsigned i = 0; i < 8; ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            worker(NULL);
            assert(munmap((void *)base, 8192) == 0);
            /* Do not call libc clocks again after deliberately unmapping. */
            _exit(0);
        }
        clean_child(child);
        worker(NULL); /* Child unmap/teardown must preserve parent's mapping. */
    }
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { execl(argv[0], argv[0], "--exec", NULL); _exit(127); }
    clean_child(child);
    worker(NULL);
    printf("VDSO_GUEST_PASS base=%p threads=8 pte_faults=3 fork_unmap=8 exec=1\n", (void *)base);
    return 0;
}

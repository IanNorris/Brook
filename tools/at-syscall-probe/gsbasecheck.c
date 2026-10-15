#define _GNU_SOURCE
#include <asm/prctl.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "GSBASECHECK FAIL line=%d errno=%d\n", __LINE__, errno); __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); } } while (0)
static unsigned long inherited;
static unsigned long getgs(void) {
    unsigned long value = ~0UL;
    CHECK(syscall(SYS_arch_prctl, ARCH_GET_GS, &value) == 0);
    return value;
}
static uint64_t loadgs(void) {
    uint64_t value;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(value) :: "memory");
    return value;
}
static void *worker(void *argument) {
    CHECK(getgs() == inherited);
    uint64_t marker = 0x4753000000000000ULL | (uintptr_t)argument;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, &marker)) {
        CHECK(0); return NULL;
    }
    for (int i = 0; i < 2000; ++i) {
        CHECK(getgs() == (unsigned long)&marker);
        CHECK(loadgs() == marker);
        sched_yield();
    }
    CHECK(syscall(SYS_arch_prctl, ARCH_SET_GS, 0UL) == 0);
    CHECK(getgs() == 0);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--exec")) {
        CHECK(getgs() == 0);
        return failures ? 1 : 0;
    }
    unsigned long original = getgs();
    uint64_t marker = 0x4753424153455445ULL;
    inherited = (unsigned long)&marker;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, inherited)) {
        CHECK(0); return 1;
    }
    CHECK(getgs() == inherited);
    CHECK(loadgs() == marker);
    unsigned long invalid[] = {0x0000800000000000UL, 0xffff800000000000UL, ~0UL};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        errno = 0;
        CHECK(syscall(SYS_arch_prctl, ARCH_SET_GS, invalid[i]) == -1 && errno == EPERM);
        CHECK(getgs() == inherited);
        errno = 0;
        CHECK(syscall(SYS_arch_prctl, ARCH_SET_FS, invalid[i]) == -1 && errno == EPERM);
    }
    errno = 0;
    CHECK(syscall(SYS_arch_prctl, ARCH_GET_GS, (void *)1) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_arch_prctl, ARCH_SET_GS, 0x00007fffffffefffUL) == 0);
    CHECK(getgs() == 0x00007fffffffefffUL);
    CHECK(syscall(SYS_arch_prctl, ARCH_SET_GS, inherited) == 0);
    pthread_t threads[4];
    int created = 0;
    for (int i = 0; i < 4; ++i) {
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1))) { CHECK(0); break; }
        ++created;
    }
    for (int i = 0; i < created; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(getgs() == inherited && loadgs() == marker);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(getgs() == inherited && loadgs() == marker);
        if (failures) _exit(1);
        execl(argv[0], argv[0], "--exec", (char *)NULL);
        _exit(127);
    }
    if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(getgs() == inherited && loadgs() == marker);
    CHECK(syscall(SYS_arch_prctl, ARCH_SET_GS, original) == 0);
    printf("GSBASECHECK %s threads=%d yields=8000 failures=%d\n", failures ? "FAIL" : "PASS", created, failures);
    return failures ? 1 : 0;
}

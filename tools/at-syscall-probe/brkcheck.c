#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile int stage;
static uintptr_t target, worker_result;
static void *worker(void *unused) {
    (void)unused;
    while (__atomic_load_n(&stage, __ATOMIC_ACQUIRE) == 0) usleep(1000);
    worker_result = syscall(SYS_brk, target);
    return NULL;
}

int main(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return 1;
    uintptr_t original = syscall(SYS_brk, 0);
    uintptr_t page = (original + 4095) & ~(uintptr_t)4095;
    uintptr_t first = page + 4096;
    target = first + 4096;
    if ((uintptr_t)syscall(SYS_brk, first) != first) return 1;
    unsigned char *marker = (unsigned char*)page;
    for (int i = 0; i < 4096; ++i) marker[i] = 0x5a;
    __atomic_store_n(&stage, 1, __ATOMIC_RELEASE);
    pthread_join(thread, NULL);
    int changed = 0;
    for (int i = 0; i < 4096; ++i) if (marker[i] != 0x5a) ++changed;
    uintptr_t observed = syscall(SYS_brk, 0);
    int failed = changed || worker_result != target || observed != target;
    syscall(SYS_brk, original);
    printf("BRKCHECK %s: %d existing heap bytes changed; shared break %s\n",
           failed ? "FAIL" : "PASS", changed, observed == target ? "matches" : "differs");
    return failed;
}

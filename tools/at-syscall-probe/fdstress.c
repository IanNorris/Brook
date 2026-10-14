#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static int source_fd, errors;
static void *worker(void *arg) {
    char byte = (char)(long)arg;
    for (int i = 0; i < 500; ++i) {
        int copy = fcntl(source_fd, F_DUPFD_CLOEXEC, 4);
        if (copy < 0) { __atomic_fetch_add(&errors, 1, __ATOMIC_RELAXED); continue; }
        if (fcntl(copy, F_GETFD) != FD_CLOEXEC || write(copy, &byte, 1) != 1)
            __atomic_fetch_add(&errors, 1, __ATOMIC_RELAXED);
        if (close(copy) != 0) __atomic_fetch_add(&errors, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

int main(void) {
    source_fd = memfd_create("brook-fd-stress", MFD_CLOEXEC);
    if (source_fd < 0 || write(source_fd, "x", 1) != 1) return 1;
    pthread_t threads[4];
    for (long i = 0; i < 4; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void*)i) != 0) return 1;
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    close(source_fd);
    printf("FDSTRESS %s: %d errors in 2000 concurrent memfd duplications\n",
           errors ? "FAIL" : "PASS", errors);
    return errors ? 1 : 0;
}

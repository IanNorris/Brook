#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Warm ext2 cache reads into inherited CoW pages run alongside lazy file
 * faults in the same child address space. No timing-only success condition:
 * every copied and mapped byte checked, all children must exit normally. */
enum { FILE_BYTES = 1024 * 1024, SPAN = 256 * 1024, THREADS = 4, CHILDREN = 4, ROUNDS = 8 };
static unsigned char *buffers;
static const char *filename;
static int failures;
static pthread_barrier_t barrier;
static unsigned char pattern(size_t offset) { return (unsigned char)((offset * 17 + offset / 4096) % 251); }
static void fail(const char *what) {
    fprintf(stderr, "CACHECOWCHECK FAIL %s errno=%d pid=%ld\n", what, errno, (long)getpid());
    __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
}
static void *worker(void *arg) {
    size_t index = (uintptr_t)arg;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { fail("open"); _exit(1); }
    pthread_barrier_wait(&barrier);
    if (index == THREADS - 1) {
        for (int round = 0; round < 8; ++round) {
            unsigned char *map = mmap(NULL, FILE_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
            if (map == MAP_FAILED) { fail("file mmap"); break; }
            for (size_t i = 0; i < FILE_BYTES; i += 4096)
                if (map[i] != pattern(i) || map[i + 4095] != pattern(i + 4095)) { fail("mapped bytes"); break; }
            if (munmap(map, FILE_BYTES)) fail("file munmap");
        }
    } else {
        for (int pass = 0; pass < 32; ++pass) {
            size_t offset = ((size_t)pass * 65536 + index * 4096) % (FILE_BYTES - 262144);
            const size_t lengths[] = {65536, 4096, 16397, 131085};
            size_t length = lengths[pass % 4];
            unsigned char *dst = buffers + index * SPAN + ((length <= 65536 && (pass & 1)) ? 128 * 1024 : 0);
            if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset || read(fd, dst, length) != (ssize_t)length) {
                fail("read"); break;
            }
            for (size_t j = 0; j < length; ++j)
                if (dst[j] != pattern(offset + j)) { fail("read bytes"); break; }
        }
        unsigned char *dst = buffers + index * SPAN;
        memset(dst, 0xa5, 65536);
        size_t tail = FILE_BYTES - 12345;
        if (lseek(fd, (off_t)tail, SEEK_SET) != (off_t)tail || read(fd, dst, 65536) != 12345)
            fail("short EOF read");
        else {
            for (size_t j = 0; j < 65536; ++j)
                if (dst[j] != (j < 12345 ? pattern(tail + j) : 0xa5)) { fail("EOF bytes/canary"); break; }
            if (lseek(fd, 0, SEEK_CUR) != FILE_BYTES || read(fd, dst, 1) != 0)
                fail("EOF offset");
        }
    }
    if (close(fd)) fail("close");
    return NULL;
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: cachecowcheck /data/CACHECOW.TST\n"); return 2; }
    filename = argv[1];
    int fd = open(filename, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) { perror("create"); return 1; }
    unsigned char block[4096];
    for (size_t off = 0; off < FILE_BYTES; off += sizeof(block)) {
        for (size_t j = 0; j < sizeof(block); ++j) block[j] = pattern(off + j);
        if (write(fd, block, sizeof(block)) != sizeof(block)) { perror("write"); return 1; }
    }
    if (fsync(fd) || lseek(fd, 0, SEEK_SET)) { perror("warm setup"); return 1; }
    for (size_t off = 0; off < FILE_BYTES; off += sizeof(block))
        if (read(fd, block, sizeof(block)) != sizeof(block)) { perror("warm read"); return 1; }
    close(fd);
    buffers = mmap(NULL, THREADS * SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffers == MAP_FAILED) { perror("buffers"); return 1; }
    memset(buffers, 0xa5, THREADS * SPAN);
    for (int round = 0; round < ROUNDS; ++round) {
        pid_t children[CHILDREN]; int created = 0;
        for (int c = 0; c < CHILDREN; ++c) {
            pid_t child = fork();
            if (child < 0) { fail("fork"); break; }
            if (!child) {
                if (pthread_barrier_init(&barrier, NULL, THREADS)) _exit(2);
                pthread_t threads[THREADS];
                for (uintptr_t t = 0; t < THREADS; ++t)
                    if (pthread_create(&threads[t], NULL, worker, (void *)t)) _exit(3);
                for (int t = 0; t < THREADS; ++t)
                    if (pthread_join(threads[t], NULL)) _exit(4);
                _exit(failures ? 1 : 0);
            }
            children[created++] = child;
        }
        for (int c = 0; c < created; ++c) {
            int status; pid_t got;
            do { got = waitpid(children[c], &status, 0); } while (got < 0 && errno == EINTR);
            if (got != children[c] || !WIFEXITED(status) || WEXITSTATUS(status)) fail("child result");
        }
        for (size_t j = 0; j < THREADS * SPAN; ++j)
            if (buffers[j] != 0xa5) { fail("parent CoW contents"); break; }
        if (failures) break;
    }
    munmap(buffers, THREADS * SPAN);
    printf("CACHECOWCHECK %s rounds=%d children=%d threads=%d failures=%d\n", failures ? "FAIL" : "PASS", ROUNDS, CHILDREN, THREADS, failures);
    return failures ? 1 : 0;
}

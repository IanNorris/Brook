#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WORKERS = 8, ROUNDS = 16, FILE_BYTES = 131119 };
static const char *fixture;
static atomic_uint ready, errors, started;
static unsigned mode;

static unsigned char pattern(unsigned offset)
{
    return (unsigned char)(offset * 37u + 11u);
}

static int verify(void)
{
    int fd = open(fixture, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    int result = fstat(fd, &st) || st.st_size != FILE_BYTES;
    unsigned char buffer[4096];
    for (unsigned offset = 0; !result && offset < FILE_BYTES;) {
        unsigned count = FILE_BYTES - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        ssize_t got = read(fd, buffer, count);
        if (got <= 0) { result = 1; break; }
        for (ssize_t i = 0; i < got; ++i)
            if (buffer[i] != pattern(offset + (unsigned)i)) result = 1;
        offset += (unsigned)got;
    }
    if (close(fd)) result = 1;
    return result ? -1 : 0;
}

static void *reader(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    int fd = open(fixture, O_RDONLY);
    if (fd < 0) atomic_fetch_add(&errors, 1);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    if (fd < 0) return NULL;
    while (!atomic_load_explicit(&started, memory_order_acquire)) sched_yield();
    if (mode == 3 && id == 0) {
        usleep(200);
        if (kill(getpid(), SIGKILL)) _exit(94);
        _exit(95); // A successful self-group kill must not return to userspace.
    }
    const unsigned offsets[] = {0, 65536, FILE_BYTES - 1};
    for (;;) {
        struct stat st;
        if (access(fixture, R_OK) || stat(fixture, &st) || st.st_size != FILE_BYTES)
            atomic_fetch_add(&errors, 1);
        for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
            unsigned char byte = 0;
            if (pread(fd, &byte, 1, offsets[i]) != 1 || byte != pattern(offsets[i]))
                atomic_fetch_add(&errors, 1);
        }
    }
}

static void child(int ready_fd, unsigned round)
{
    atomic_store(&ready, 0);
    atomic_store(&errors, 0);
    atomic_store(&started, 0);
    mode = round % 4;
    pthread_t threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i)
        if (pthread_create(&threads[i], NULL, reader, (void *)(uintptr_t)i)) _exit(90);
    while (atomic_load_explicit(&ready, memory_order_acquire) != WORKERS) sched_yield();
    if (atomic_load(&errors)) _exit(91);
    if (write(ready_fd, "R", 1) != 1) _exit(92);
    close(ready_fd);
    atomic_store_explicit(&started, 1, memory_order_release);
    if (mode == 1 || mode == 3) {
        for (;;) usleep(10000); // Parent or worker sends process-directed SIGKILL.
    }
    usleep(50 + round * 50);
    if (mode == 2) {
        if (kill(getpid(), SIGKILL)) _exit(94);
        _exit(95);
    }
    _exit(atomic_load(&errors) ? 93 : 43); // Deliberately leave live I/O threads.
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: fsexitcheck PATH [--verify]\n");
        return 2;
    }
    fixture = argv[1];
    // musl implements alarm through setitimer, which Brook does not yet expose.
    // Use the implemented Linux x86-64 alarm syscall; retain a host deadline too
    // because a broken filesystem can also prevent fatal-signal completion.
    if (syscall(SYS_alarm, 45) < 0) { perror("alarm"); return 1; }
    if (argc == 3) {
        if (strcmp(argv[2], "--verify")) return 2;
        int result = verify();
        printf("FSEXITCHECK VERIFY %s bytes=%u\n", result ? "FAIL" : "PASS", FILE_BYTES);
        return result != 0;
    }
    int fd = open(fixture, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) { perror("fixture open"); return 1; }
    unsigned char buffer[4096];
    for (unsigned offset = 0; offset < FILE_BYTES;) {
        unsigned count = FILE_BYTES - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        for (unsigned i = 0; i < count; ++i) buffer[i] = pattern(offset + i);
        ssize_t got = write(fd, buffer, count);
        if (got <= 0) { perror("fixture write"); close(fd); return 1; }
        offset += (unsigned)got;
    }
    if (fsync(fd) || close(fd)) { perror("fixture sync/close"); return 1; }
    for (unsigned round = 0; round < ROUNDS; ++round) {
        int pipefd[2];
        if (pipe(pipefd)) { perror("pipe"); return 1; }
        pid_t pid = fork();
        if (!pid) { close(pipefd[0]); child(pipefd[1], round); }
        close(pipefd[1]);
        if (pid < 0) { close(pipefd[0]); perror("fork"); return 1; }
        char message = 0;
        int notified = read(pipefd[0], &message, 1) == 1 && message == 'R';
        close(pipefd[0]);
        if (round % 4 == 1 && notified) {
            usleep(50 + round * 50);
            if (kill(pid, SIGKILL)) { perror("kill"); return 1; }
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid || !notified ||
            ((round % 4) ? !(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                         : !(WIFEXITED(status) && WEXITSTATUS(status) == 43))) {
            fprintf(stderr, "FSEXITCHECK FAIL round=%u status=%d notified=%d\n", round, status, notified);
            return 1;
        }
        // This must acquire the filesystem again after every abrupt group exit.
        if (verify()) { fprintf(stderr, "FSEXITCHECK FAIL after round=%u errno=%d\n", round, errno); return 1; }
    }
    printf("FSEXITCHECK PASS rounds=%u workers=%u bytes=%u path=%s\n", ROUNDS, WORKERS, FILE_BYTES, fixture);
    return 0;
}

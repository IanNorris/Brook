#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The contract is intentionally Linux behavior, including truncate while mapped.
 * Brook's current rejection is a failure, not an alternate passing mode. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FILESHAREDCHECK FAIL line=%d expr=%s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static volatile sig_atomic_t active_child;
static void reap_child(void) {
    pid_t pid = active_child;
    if (pid > 0) {
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        active_child = 0;
    }
}
static void expired(int signal_number) {
    (void)signal_number;
    if (active_child > 0) kill(active_child, SIGKILL);
    _exit(124);
}
static void byte(int fd, char value) { CHECK(write(fd, &value, 1) == 1); }
static void expect(int fd, char value) { char got = 0; CHECK(read(fd, &got, 1) == 1 && got == value); }
static void wait_ok(pid_t pid) { int status; CHECK(waitpid(pid, &status, 0) == pid); active_child = 0; CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); }

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(atexit(reap_child) == 0);
    struct sigaction action = {0}; action.sa_handler = expired;
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    timer_t timer;
    struct sigevent event = {0}; event.sigev_notify = SIGEV_SIGNAL; event.sigev_signo = SIGALRM;
    CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
    struct itimerspec deadline = {0}; deadline.it_value.tv_sec = 60;
    CHECK(timer_settime(timer, 0, &deadline, NULL) == 0);
    long page = sysconf(_SC_PAGESIZE); CHECK(page == 4096);
    int fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR, 0600); CHECK(fd >= 0);
    CHECK(ftruncate(fd, page * 2) == 0);
    int other = open(argv[1], O_RDWR); CHECK(other >= 0);
    volatile unsigned char *a = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(a != MAP_FAILED);
    volatile unsigned char *b = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_SHARED, other, 0);
    CHECK(b != MAP_FAILED);
    int ready[2], proceed[2]; CHECK(pipe(ready) == 0 && pipe(proceed) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    active_child = child;
    if (!child) {
        close(ready[0]); close(proceed[1]);
        /* Write a separate alias after fork, with both pages initially untouched. */
        b[page + 17] = 0x62; byte(ready[1], 'r'); expect(proceed[0], 'g');
        CHECK(b[17] == 0x41 && b[page + 17] == 0x62);
        _exit(0);
    }
    close(ready[1]); close(proceed[0]);
    a[17] = 0x41; expect(ready[0], 'r'); byte(proceed[1], 'g'); wait_ok(child);
    close(ready[0]); close(proceed[1]);
    CHECK(a[page + 17] == 0x62 && b[17] == 0x41);
    unsigned char value = 0;
    CHECK(pread(other, &value, 1, 17) == 1 && value == 0x41);
    value = 0x73; CHECK(pwrite(other, &value, 1, page + 31) == 1);
    CHECK(a[page + 31] == value && b[page + 31] == value);
    unsigned char *private = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(private != MAP_FAILED && private[17] == 0x41);
    private[17] = 0x99; CHECK(a[17] == 0x41 && b[17] == 0x41);
    CHECK(munmap(private, page * 2) == 0);
    CHECK(msync((void *)a, page * 2, MS_SYNC) == 0 && fsync(fd) == 0);
    CHECK(close(fd) == 0 && close(other) == 0);
    fd = open(argv[1], O_RDWR); CHECK(fd >= 0);
    CHECK(pread(fd, &value, 1, page + 17) == 1 && value == 0x62);
    /* Partial EOF remains mapped and zero-filled; whole pages beyond EOF bus fault. */
    CHECK(ftruncate(fd, page + 8) == 0);
    CHECK(a[page + 17] == 0 && b[page + 31] == 0);
    CHECK(ftruncate(fd, page) == 0);
    child = fork(); CHECK(child >= 0);
    active_child = child;
    if (!child) { signal(SIGBUS, SIG_DFL); value = a[page]; _exit(value == 0 ? 10 : 11); }
    int status; CHECK(waitpid(child, &status, 0) == child);
    active_child = 0;
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS);
    CHECK(ftruncate(fd, page * 2) == 0);
    CHECK(a[page + 17] == 0 && b[page + 31] == 0);
    /* An unlinked, closed backing remains alive through both aliases. */
    CHECK(unlink(argv[1]) == 0 && close(fd) == 0);
    a[page + 17] = 0x84; CHECK(b[page + 17] == 0x84);
    CHECK(msync((void *)a, page * 2, MS_SYNC) == 0);
    CHECK(munmap((void *)a, page * 2) == 0 && munmap((void *)b, page * 2) == 0);
    CHECK(timer_delete(timer) == 0);
    puts("FILESHAREDCHECK PASS aliases/fork/io/cow/msync/truncate/eof/unlink");
    return 0;
}

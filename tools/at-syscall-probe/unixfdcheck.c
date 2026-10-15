#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int sockets[2];
static atomic_int failures, ready, start;
static void *churn(void *opaque)
{
    long id = (long)opaque;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&start)) sched_yield();
    for (int i = 0; i < 500; ++i) {
        int fd = (i & 1) ? dup(sockets[0]) : fcntl(sockets[0], F_DUPFD_CLOEXEC, 32);
        if (fd < 0) { atomic_fetch_add(&failures, 1); continue; }
        if (fcntl(fd, F_GETFD) < 0) atomic_fetch_add(&failures, 1);
        close(fd);
        // Several threads deliberately race replacement/close of one slot.
        // Linux and Brook may report EBUSY during a dup2/open race.
        fd = (id & 1) ? dup2(sockets[0], 200) : dup3(sockets[0], 200, O_CLOEXEC);
        if (fd < 0 && errno != EBUSY) atomic_fetch_add(&failures, 1);
        if (fd >= 0) close(fd);
    }
    return NULL;
}
int main(void)
{
    syscall(SYS_alarm, 45);
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
        perror("socketpair"); return 1;
    }
    pthread_t threads[4];
    for (long i = 0; i < 4; ++i)
        if (pthread_create(&threads[i], NULL, churn, (void *)i)) return 1;
    while (atomic_load(&ready) != 4) sched_yield();
    atomic_store(&start, 1);
    for (int i = 0; i < 24; ++i) {
        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }
        if (!child) {
            // Parent's socket must survive all inherited descriptor closes.
            close(sockets[0]); close(sockets[1]);
            _exit(0);
        }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
            atomic_fetch_add(&failures, 1);
    }
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    close(200);
    char sent = 'S', received = 0;
    if (write(sockets[0], &sent, 1) != 1 || read(sockets[1], &received, 1) != 1 || received != sent)
        atomic_fetch_add(&failures, 1);
    int survivor = dup(sockets[0]);
    close(sockets[0]);
    sent = 'T'; received = 0;
    if (survivor < 0 || write(survivor, &sent, 1) != 1 ||
        read(sockets[1], &received, 1) != 1 || received != sent)
        atomic_fetch_add(&failures, 1);
    close(survivor); close(sockets[1]);
    int f = atomic_load(&failures);
    printf("UNIXFDCHECK %s threads=4 iterations=500 forks=24 failures=%d\n", f ? "FAIL" : "PASS", f);
    return f ? 1 : 0;
}

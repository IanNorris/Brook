#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <sys/sem.h>
#include <sys/syscall.h>
#include <time.h>
#include <sched.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };
static volatile sig_atomic_t owned = -1;
static void cleanup(void) {
    if (owned >= 0) { semctl(owned, 0, IPC_RMID); owned = -1; }
}
static void expired(int signal_number) {
    (void)signal_number;
    cleanup();
    static const char message[] = "SYSV_SEM_PROBE timeout\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message)-1);
    _exit(written == (ssize_t)(sizeof(message)-1) ? 124 : 125);
}
static void require(int condition, const char *stage) {
    if (condition) return;
    fprintf(stderr, "SYSV_SEM_PROBE_FAIL stage=%s errno=%d\n", stage, errno);
    exit(1);
}
static void reap(pid_t child, const char *stage) {
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0, stage);
}
static int host_unmapped(const char *path, unsigned long id) {
    FILE *map = fopen(path, "r");
    if (!map) return 0;
    unsigned long first, outside, count;int mapped = 0, rows = 0;
    while (fscanf(map, "%lu %lu %lu", &first, &outside, &count) == 3) {
        ++rows;if (id >= first && id-first < count) mapped = 1;
    }
    fclose(map);return rows && !mapped;
}
static void interrupted(int signal_number) {(void)signal_number;}
static int shared_undo_child(void *unused) {
    (void)unused;
    struct sembuf take = {0, -1, SEM_UNDO};
    _exit(semop(owned, &take, 1) == 0 ? 0 : 41);
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--undo-exec")) {
        int id = atoi(argv[2]);
        _exit(semctl(id, 0, GETVAL) == 0 ? 0 : 42);
    }
    atexit(cleanup);
    struct sigaction action = {0};
    action.sa_handler = expired;
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGALRM, &action, NULL) == 0, "timeout-handler");
    alarm(15);
    owned = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    require(owned >= 0, "create-private-set");
    unsigned short values[2] = {2, 0};
    union semun argument = {.array = values};
    require(semctl(owned, 0, SETALL, argument) == 0, "setall");
    struct semid_ds info;
    argument.buf = &info;
    require(semctl(owned, 0, IPC_STAT, argument) == 0 &&
            info.sem_nsems == 2 && (info.sem_perm.mode & 0777) == 0600 &&
            info.sem_perm.uid == geteuid(), "stat-owner-geometry");

    // IPC creator authority permits ownership transfer without chown membership rules.
    info.sem_perm.uid = geteuid() + 1;info.sem_perm.gid = getegid() + 1;
    int transfer = semctl(owned, 0, IPC_SET, argument);
    int transfer_tested = transfer == 0;
    if (transfer < 0 && errno == EINVAL &&
        (host_unmapped("/proc/self/uid_map", geteuid()+1) ||
         host_unmapped("/proc/self/gid_map", getegid()+1))) {
        fputs("SYSV_SEM_PROBE_SKIP ownership transfer: host namespace has no alternate mapped IDs\n", stderr);
    } else require(transfer == 0, "creator-ownership-transfer");
    if (transfer_tested)
        require(semctl(owned, 0, IPC_STAT, argument) == 0 &&
                info.sem_perm.uid == geteuid() + 1 && info.sem_perm.cuid == geteuid(),
                "creator-identity-retained");
    info.sem_perm.uid = (uid_t)-1;errno = 0;
    require(semctl(owned, 0, IPC_SET, argument) == -1 && errno == EINVAL,
            "invalid-owner-rejected");
    info.sem_perm.uid = geteuid();info.sem_perm.gid = getegid();
    require(semctl(owned, 0, IPC_SET, argument) == 0, "restore-owner");

    struct sembuf failed[2] = {{0, -1, IPC_NOWAIT}, {1, -1, IPC_NOWAIT}};
    errno = 0;
    require(semop(owned, failed, 2) == -1 && errno == EAGAIN,
            "atomic-would-block");
    require(semctl(owned, 0, GETVAL) == 2 && semctl(owned, 1, GETVAL) == 0,
            "atomic-no-partial-decrement");
    struct sembuf ready[3] = {{0, -2, 0}, {1, 1, 0}, {0, 0, 0}};
    require(semop(owned, ready, 3) == 0 && semctl(owned, 0, GETVAL) == 0 &&
            semctl(owned, 1, GETVAL) == 1, "ordered-atomic-zero-wait");
    struct sembuf invalid = {2, 1, IPC_NOWAIT};
    errno = 0;
    require(semop(owned, &invalid, 1) == -1 && errno == EFBIG,
            "invalid-semaphore-index");

    struct sembuf timed = {0, -1, 0};
    struct timespec timeout = {0, 20000000}, original_timeout = timeout;
    errno = 0;
    require(semtimedop(owned, &timed, 1, &timeout) == -1 && errno == EAGAIN,
            "timed-wait-expires");
    require(!memcmp(&timeout, &original_timeout, sizeof(timeout)) &&
            semctl(owned, 0, GETNCNT) == 0, "timed-wait-unlinks-preserves-timeout");
    // Other semaphore updates wake the waiter but must not renew its timeout.
    pid_t waker = fork();require(waker >= 0, "timed-waker-fork");
    if (!waker) {
        struct sembuf update[2] = {{1, 1, 0}, {1, -1, 0}};
        for (unsigned i = 0; i < 1000; ++i) {
            if (semop(owned, update, 2)) _exit(53);
            usleep(2000);
        }
        _exit(0);
    }
    struct timespec before, after;
    require(clock_gettime(CLOCK_MONOTONIC, &before) == 0, "timed-clock-before");
    errno = 0;
    require(semtimedop(owned, &timed, 1, &timeout) == -1 && errno == EAGAIN,
            "timed-wait-unrelated-wakes");
    require(clock_gettime(CLOCK_MONOTONIC, &after) == 0, "timed-clock-after");
    long long elapsed = (long long)(after.tv_sec-before.tv_sec)*1000000000LL +
                        after.tv_nsec-before.tv_nsec;
    reap(waker, "timed-waker-complete");
    require(elapsed >= 20000000 && elapsed < 1000000000LL,
            "timed-wait-original-deadline");
    timeout.tv_nsec = 1000000000;errno = 0;
    require(semtimedop(owned, &timed, 1, &timeout) == -1 && errno == EINVAL,
            "invalid-timeout");
    timed.sem_op = 1;
    require(syscall(SYS_semop, owned, &timed, 1) == 0 &&
            semctl(owned, 0, GETVAL) == 1, "direct-semop-entry");

    argument.val = 1;
    require(semctl(owned, 0, SETVAL, argument) == 0, "undo-initialize");
    pid_t child = fork();
    require(child >= 0, "undo-fork");
    if (!child) {
        struct sembuf take = {0, -1, SEM_UNDO};
        if (semop(owned, &take, 1) || semctl(owned, 0, GETVAL) != 0) _exit(31);
        _exit(0);
    }
    siginfo_t dead = {0};
    require(waitid(P_PID, child, &dead, WEXITED | WNOWAIT) == 0 &&
            dead.si_pid == child, "undo-wnowait");
    require(semctl(owned, 0, GETVAL) == 1, "undo-before-wnowait-observation");
    reap(child, "undo-child-exit");
    require(semctl(owned, 0, GETVAL) == 1, "undo-on-process-exit");

    child = fork();
    require(child >= 0, "exec-undo-fork");
    if (!child) {
        struct sembuf take = {0, -1, SEM_UNDO};
        if (semop(owned, &take, 1)) _exit(43);
        char text[32];snprintf(text, sizeof(text), "%d", (int)owned);
        char *args[] = {argv[0], "--undo-exec", text, NULL};
        execv(argv[0], args);_exit(44);
    }
    reap(child, "undo-preserved-through-exec");
    require(semctl(owned, 0, GETVAL) == 1, "exec-undo-on-exit");

    argument.val = 2;
    require(semctl(owned, 0, SETVAL, argument) == 0, "shared-undo-initialize");
    child = fork();
    require(child >= 0, "shared-undo-fork");
    if (!child) {
        void *stack = malloc(65536);
        if (!stack) _exit(45);
        // Share before either participant has used SEM_UNDO.
        pid_t shared = clone(shared_undo_child, (char *)stack + 65536,
                             CLONE_SYSVSEM | SIGCHLD, NULL);
        int status = 0;
        if (shared < 0 || waitpid(shared, &status, 0) != shared ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) _exit(46);
        if (semctl(owned, 0, GETVAL) != 1) _exit(47);
        struct sembuf take = {0, -1, SEM_UNDO};
        if (semop(owned, &take, 1)) _exit(48);
        free(stack);_exit(0);
    }
    reap(child, "shared-undo-last-alias");
    require(semctl(owned, 0, GETVAL) == 2, "shared-undo-applied-once");

    argument.val = 1;
    require(semctl(owned, 0, SETVAL, argument) == 0, "fatal-undo-initialize");
    child = fork();require(child >= 0, "fatal-undo-fork");
    if (!child) {
        struct sembuf take = {0, -1, SEM_UNDO};
        if (semop(owned, &take, 1)) _exit(49);
        struct sembuf block = {0, -1, 0};
        (void)semop(owned, &block, 1);_exit(50);
    }
    unsigned fatal_tries;
    for (fatal_tries = 0; fatal_tries < 2000; ++fatal_tries) {
        if (semctl(owned, 0, GETNCNT) == 1) break;
        usleep(1000);
    }
    require(fatal_tries < 2000, "fatal-waiter-registered");
    require(kill(child, SIGKILL) == 0, "fatal-kill");
    int killed_status = 0;
    require(waitpid(child, &killed_status, 0) == child &&
            WIFSIGNALED(killed_status) && WTERMSIG(killed_status) == SIGKILL,
            "fatal-waiter-reaped");
    require(semctl(owned, 0, GETVAL) == 1 && semctl(owned, 0, GETNCNT) == 0,
            "fatal-undo-and-waiter-cleanup");

    argument.val = 0;
    require(semctl(owned, 0, SETVAL, argument) == 0, "interrupt-initialize");
    child = fork();require(child >= 0, "interrupt-fork");
    if (!child) {
        struct sigaction caught = {0};caught.sa_handler = interrupted;
        sigemptyset(&caught.sa_mask);
        if (sigaction(SIGUSR1, &caught, NULL)) _exit(51);
        struct sembuf block = {0, -1, 0};errno = 0;
        int r = semop(owned, &block, 1);
        _exit(r == -1 && errno == EINTR ? 0 : 52);
    }
    unsigned interrupt_tries;
    for (interrupt_tries = 0; interrupt_tries < 2000; ++interrupt_tries) {
        if (semctl(owned, 0, GETNCNT) == 1) break;
        usleep(1000);
    }
    require(interrupt_tries < 2000, "interrupt-waiter-registered");
    require(kill(child, SIGUSR1) == 0, "interrupt-signal");
    reap(child, "interrupted-eintr");
    require(semctl(owned, 0, GETNCNT) == 0, "interrupted-waiter-unlinked");

    argument.val = 0;
    require(semctl(owned, 0, SETVAL, argument) == 0, "wait-initialize");
    child = fork();
    require(child >= 0, "wait-fork");
    if (!child) {
        struct sembuf take = {0, -1, 0};
        errno = 0;
        int result = semop(owned, &take, 1);
        _exit(result == -1 && errno == EIDRM ? 0 : 32);
    }
    unsigned tries;
    for (tries = 0; tries < 2000; ++tries) {
        int waiters = semctl(owned, 0, GETNCNT);
        require(waiters >= 0, "query-waiters");
        if (waiters == 1) break;
        usleep(1000);
    }
    require(tries < 2000, "waiter-registered");
    int stale = owned;
    require(semctl(owned, 0, IPC_RMID) == 0, "remove-wakes-waiter");
    owned = -1;
    reap(child, "removed-waiter-eidrm");
    errno = 0;
    require(semctl(stale, 0, GETVAL) == -1 && errno == EINVAL,
            "removed-id-rejected");
    alarm(0);
    printf("{\"ownership_transfer_tested\":%s,\"test\":\"sysv-sem\",\"passed\":true,\"scope\":\"private set, permissions metadata, atomic operations, wait-for-zero, SEM_UNDO fork/clone/exec/fatal exit and WNOWAIT ordering, interrupted waits, removal wake and stale ID; not full IPC compatibility\"}\n", transfer_tested ? "true" : "false");
    return 0;
}

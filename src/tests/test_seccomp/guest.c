#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <pthread.h>
#include <sched.h>
#include <seccomp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "SECCOMP_FIXTURE_FAIL line=%d expression=%s errno=%d\n", \
            __LINE__, #condition, errno); exit(2); \
} } while (0)

static const char *program;
static _Atomic int state;
static _Atomic long workerResult;
static _Atomic long workerNnp;
static volatile sig_atomic_t trapSeen;

static int install(struct sock_filter *instructions, unsigned short count, unsigned flags)
{
    struct sock_fprog filter = {.len = count, .filter = instructions};
    return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, flags, &filter);
}

static int blocked(long number, long argument, int code)
{
    errno = 0;
    return syscall(number, argument) == -1 && errno == code;
}

static void trap(int signal, siginfo_t *info, void *frame)
{
    ucontext_t *context = frame;
    trapSeen = signal == SIGSYS && info->si_code == 1 && info->si_errno == 0x1234 &&
        info->si_syscall == SYS_getuid && info->si_arch == AUDIT_ARCH_X86_64 &&
        info->si_call_addr == (void *)context->uc_mcontext.gregs[REG_RIP] &&
        context->uc_mcontext.gregs[REG_RAX] == SYS_getuid;
    context->uc_mcontext.gregs[REG_RAX] = 12345;
}

static void *worker(void *conflict)
{
    if (conflict) {
        struct sock_filter allow[] = {BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
        CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
        CHECK(install(allow, 1, 0) == 0);
        atomic_store(&workerResult, syscall(SYS_gettid));
    }
    atomic_store(&state, 1);
    while (atomic_load(&state) < 2) sched_yield();
    if (!conflict) {
        atomic_store(&workerNnp, prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L));
        atomic_store(&workerResult, blocked(SYS_getppid, 0, ENOTTY));
    }
    return NULL;
}

static void synchronized(void)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    while (atomic_load(&state) != 1) sched_yield();
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
    scmp_filter_ctx policy = seccomp_init(SCMP_ACT_ALLOW);
    CHECK(policy);
    CHECK(seccomp_attr_set(policy, SCMP_FLTATR_CTL_TSYNC, 1) == 0);
    CHECK(seccomp_rule_add(policy, SCMP_ACT_ERRNO(ENOTTY), SCMP_SYS(getppid), 0) == 0);
    const int result = seccomp_load(policy);
    if (result) fprintf(stderr, "SEC_SCMP_LOAD=%d\n", result);
    CHECK(result == 0);
    seccomp_release(policy);
    CHECK(blocked(SYS_getppid, 0, ENOTTY));
    atomic_store(&state, 2);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(atomic_load(&workerResult) == 1 && atomic_load(&workerNnp) == 1);
    fprintf(stderr, "SECCOMP_TSYNC_PASS\n");
}

static void privileged_tsync(void)
{
    CHECK(geteuid() == 0);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    while (atomic_load(&state) != 1) sched_yield();
    struct sock_filter policy[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOTTY),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(policy, 4, SECCOMP_FILTER_FLAG_TSYNC) == 0);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == 0);
    CHECK(blocked(SYS_getppid, 0, ENOTTY));
    atomic_store(&state, 2);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(atomic_load(&workerResult) == 1 && atomic_load(&workerNnp) == 0);
    fprintf(stderr, "SECCOMP_PRIVILEGED_TSYNC_PASS\n");
}

static void conflicting(void)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, (void *)1) == 0);
    while (atomic_load(&state) != 1) sched_yield();
    struct sock_filter candidate[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
    CHECK(install(candidate, 4, SECCOMP_FILTER_FLAG_TSYNC) == atomic_load(&workerResult));
    CHECK(install(candidate, 4, SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_TSYNC_ESRCH) == -1 &&
          errno == ESRCH);
    CHECK(syscall(SYS_getppid) > 0);
    atomic_store(&state, 2);
    CHECK(pthread_join(thread, NULL) == 0);
    fprintf(stderr, "SECCOMP_DIVERGENCE_PASS\n");
}

static void expect_kill(uint32_t action)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct sock_filter code[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getuid, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, action),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        signal(SIGSYS, SIG_IGN);
        CHECK(install(code, 4, 0) == 0);
        syscall(SYS_getuid);
        _exit(3);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS);
}

static void reviewed_actions(void)
{
    uint32_t action = SECCOMP_RET_LOG;
    CHECK(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0);
    action = SECCOMP_RET_TRACE;
    CHECK(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0);
    action = SECCOMP_RET_USER_NOTIF;
    CHECK(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == -1 &&
          errno == EOPNOTSUPP);
    struct sock_filter policy[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_gettid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_LOG),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE | 12),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getsid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(policy, 8, 1u << 31) == -1 && errno == EINVAL);
    CHECK(install(policy, 8, SECCOMP_FILTER_FLAG_NEW_LISTENER) == -1 && errno == EINVAL);
    CHECK(install(policy, 8, SECCOMP_FILTER_FLAG_SPEC_ALLOW | SECCOMP_FILTER_FLAG_LOG) == 0);
    CHECK(syscall(SYS_gettid) > 0);
    usleep(150000);
    CHECK(blocked(SYS_getppid, 0, ENOSYS));
    CHECK(blocked(SYS_getsid, 0, ENOSYS));

    struct sock_filter unknown[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0x7ffe0000),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(unknown, 4, 0) == 0);
    CHECK(blocked(SYS_getppid, 0, ENOSYS)); // The older TRACE outranks the unknown value.
    expect_kill(0x7ffe0000);                // The same unknown value kills when it wins.
    fprintf(stderr, "SECCOMP_REVIEW_ACTIONS_PASS\n");
}

static scmp_filter_ctx fixture_policy(void)
{
    scmp_filter_ctx policy = seccomp_init(SCMP_ACT_ALLOW);
    CHECK(policy);
    CHECK(seccomp_rule_add(policy, SCMP_ACT_ERRNO(ENOTTY), SCMP_SYS(getpgid), 1,
                           SCMP_A0(SCMP_CMP_EQ, 0)) == 0);
    CHECK(seccomp_rule_add(policy, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_GE, 40)) == 0);
    CHECK(seccomp_rule_add(policy, SCMP_ACT_ERRNO(EACCES), SCMP_SYS(ioctl), 1,
                           SCMP_A1(SCMP_CMP_MASKED_EQ, 0xff00, 0x5400)) == 0);
    return policy;
}

int main(int argc, char **argv)
{
    program = argv[0];
    if (argc == 2 && strcmp(argv[1], "--export") == 0) {
        scmp_filter_ctx policy = fixture_policy();
        CHECK(seccomp_export_bpf(policy, STDOUT_FILENO) == 0);
        seccomp_release(policy);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--sandboxed") == 0) {
        CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == SECCOMP_MODE_FILTER);
        CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == 1);
        CHECK(blocked(SYS_getpgid, 0, ENOTTY));
        CHECK(syscall(SYS_getpid) > 0);
        fprintf(stderr, "SECCOMP_BWRAP_PASS\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--inherited") == 0) {
        CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == SECCOMP_MODE_FILTER);
        CHECK(blocked(SYS_getpgid, 0, EHOSTUNREACH));
        fprintf(stderr, "SECCOMP_EXEC_INHERIT_PASS\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--tsync") == 0) {
        synchronized();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--privileged") == 0) {
        privileged_tsync();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--review-actions") == 0) {
        CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
        reviewed_actions();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--conflict") == 0) {
        conflicting();
        return 0;
    }

    struct sock_filter allow[] = {BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
    struct sock_filter bad[] = {
        BPF_JUMP(BPF_JMP | BPF_JA, UINT32_MAX, 0, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == 0);
    CHECK(install(allow, 1, 0) == -1 && errno == EACCES);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
    CHECK(install(bad, 2, 0) == -1 && errno == EINVAL);

    scmp_filter_ctx policy = fixture_policy();
    CHECK(seccomp_load(policy) == 0);
    seccomp_release(policy);
    CHECK(blocked(SYS_getpgid, 0, ENOTTY));
    CHECK(syscall(SYS_getpgid, syscall(SYS_getpid)) > 0);
    CHECK(socket(40, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT);
    int stream = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(stream >= 0);
    CHECK(syscall(SYS_ioctl, stream, 0x5413, 0) == -1 && errno == EACCES);
    close(stream);

    struct sock_filter second[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getpgid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EHOSTUNREACH),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog secondProgram = {.len = 4, .filter = second};
    CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &secondProgram, 0L, 0L) == 0);
    CHECK(blocked(SYS_getpgid, 0, EHOSTUNREACH));

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(blocked(SYS_getpgid, 0, EHOSTUNREACH));
        execl(program, program, "--inherited", (char *)NULL);
        _exit(4);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    expect_kill(SECCOMP_RET_KILL_THREAD);
    expect_kill(SECCOMP_RET_KILL_PROCESS);

    struct sigaction handler = {.sa_sigaction = trap, .sa_flags = SA_SIGINFO};
    sigemptyset(&handler.sa_mask);
    CHECK(sigaction(SIGSYS, &handler, NULL) == 0);
    struct sock_filter traps[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getuid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | 0x1234),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(traps, 4, 0) == 0);
    CHECK(syscall(SYS_getuid) == 12345 && trapSeen);
    fprintf(stderr, "SECCOMP_FIXTURE_PASS\n");
    return 0;
}

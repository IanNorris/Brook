/* Guest check for compat batch 1 (setitimer/getitimer, kcmp, madvise fork
 * advice, sysfs CPU lists + /proc/self/cgroup, TCP_INFO, quiet rseq).
 * Linux is the oracle: this program passes unchanged on a Linux host.
 * Build: x86_64-unknown-linux-musl-gcc -static -O2 -o compat_batch1_check compat_batch1_check.c */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s (errno=%d)\n", __func__, __LINE__, #x, errno); ++failures; } } while (0)

static volatile sig_atomic_t alarms, vtalarms, profs;
static void on_alarm(int s) { (void)s; ++alarms; }
static void on_vtalarm(int s) { (void)s; ++vtalarms; }
static void on_prof(int s) { (void)s; ++profs; }
static struct itimerval tv(long value_us, long interval_us)
{
    struct itimerval t = {{interval_us / 1000000, interval_us % 1000000}, {value_us / 1000000, value_us % 1000000}};
    return t;
}
static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

static void itimers(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = on_vtalarm; sigaction(SIGVTALRM, &sa, NULL);
    sa.sa_handler = on_prof; sigaction(SIGPROF, &sa, NULL);
    struct itimerval value = tv(20000, 20000), old, now;
    CHECK(setitimer(ITIMER_REAL, &value, &old) == 0 && !old.it_value.tv_sec && !old.it_value.tv_usec);
    CHECK(getitimer(ITIMER_REAL, &now) == 0 && now.it_interval.tv_usec == 20000);
    sleep_ms(300); /* interrupted by each SIGALRM, resumed */
    CHECK(alarms >= 5);
    pid_t child = fork();
    if (!child) { struct itimerval c; _exit(getitimer(ITIMER_REAL, &c) || c.it_value.tv_sec || c.it_value.tv_usec); }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    value = tv(0, 0);
    CHECK(setitimer(ITIMER_REAL, &value, &old) == 0 && old.it_interval.tv_usec == 20000);
    int seen = alarms; sleep_ms(80); CHECK(alarms == seen); /* disarmed */
    /* alarm() shares ITIMER_REAL. */
    CHECK(alarm(7) == 0);
    CHECK(getitimer(ITIMER_REAL, &now) == 0 && now.it_value.tv_sec >= 6);
    CHECK(alarm(0) == 7);
    value = tv(0, 0); value.it_value.tv_usec = 1000000; /* invalid usec */
    CHECK(setitimer(ITIMER_REAL, &value, NULL) == -1 && errno == EINVAL);
    CHECK(setitimer(3, &value, NULL) == -1 && errno == EINVAL);
    /* CPU timers: VIRTUAL counts user time, PROF user+system. */
    value = tv(10000, 10000);
    CHECK(setitimer(ITIMER_VIRTUAL, &value, NULL) == 0);
    CHECK(setitimer(ITIMER_PROF, &value, NULL) == 0);
    for (volatile unsigned long i = 0; (vtalarms < 3 || profs < 3) && i < 4000000000UL; ++i) {
        if (!(i & 0xfffff)) getppid(); /* a syscall return point for delivery */
    }
    CHECK(vtalarms >= 3 && profs >= 3);
    value = tv(0, 0);
    setitimer(ITIMER_VIRTUAL, &value, NULL); setitimer(ITIMER_PROF, &value, NULL);
    printf("itimer: alarms=%d vtalrm=%d prof=%d\n", (int)alarms, (int)vtalarms, (int)profs);
}

static long kcmp(pid_t a, pid_t b, int type, unsigned long x, unsigned long y)
{
    return syscall(312, a, b, type, x, y);
}
static void kcmps(void)
{
    pid_t self = getpid();
    int a = open("/proc/self/cgroup", O_RDONLY), b = dup(a), c = open("/proc/self/cgroup", O_RDONLY);
    CHECK(a >= 0 && b >= 0 && c >= 0);
    CHECK(kcmp(self, self, 0, a, b) == 0);
    long order = kcmp(self, self, 0, a, c);
    CHECK(order == 1 || order == 2);
    CHECK(kcmp(self, self, 0, c, a) == (order ^ 3));
    CHECK(kcmp(self, self, 0, a, 999) == -1 && errno == EBADF);
    CHECK(kcmp(self, self, 1, 0, 0) == 0);
    CHECK(kcmp(self, self, 99, 0, 0) == -1 && errno == EINVAL);
    CHECK(kcmp(self, 0x7ffffff0, 1, 0, 0) == -1 && errno == ESRCH);
    int pipefd[2]; CHECK(pipe(pipefd) == 0);
    pid_t child = fork();
    if (!child) { char ch; close(pipefd[1]); read(pipefd[0], &ch, 1); _exit(0); }
    CHECK(kcmp(self, child, 0, a, a) == 0);    /* inherited description */
    CHECK(kcmp(self, child, 1, 0, 0) != 0);    /* separate VM */
    CHECK(kcmp(self, child, 2, 0, 0) != 0);    /* separate fd table */
    close(pipefd[1]);
    int status; waitpid(child, &status, 0);
    close(a); close(b); close(c); close(pipefd[0]);
    printf("kcmp: order=%ld\n", order);
}

static void fork_advice(void)
{
    const long page = 4096;
    char* m = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    memset(m, 0x5a, 3 * page);
    CHECK(madvise(m + page, page, MADV_DONTFORK) == 0);
    CHECK(madvise(m + 2 * page, page, MADV_WIPEONFORK) == 0);
    CHECK(madvise(m, page, 102 /* MADV_GUARD_INSTALL */) == -1 && errno == EINVAL);
    pid_t child = fork();
    if (!child) {
        if (m[0] != 0x5a) _exit(2);
        if (m[2 * page] != 0) _exit(3);   /* wiped */
        volatile char x = m[page];        /* not inherited: SIGSEGV */
        (void)x; _exit(4);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(m[page] == 0x5a && m[2 * page] == 0x5a); /* parent keeps its pages */
    CHECK(madvise(m + page, page, MADV_DOFORK) == 0 && madvise(m + 2 * page, page, MADV_KEEPONFORK) == 0);
    child = fork();
    if (!child) _exit(m[page] == 0x5a && m[2 * page] == 0x5a ? 0 : 5);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    munmap(m, 3 * page);
    printf("madvise: dontfork/wipeonfork ok\n");
}

static int read_text(const char* path, char* out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    return (int)n;
}
static void sysfs(void)
{
    char possible[32], present[32], online[32], kmax[32], cgroup[64];
    CHECK(read_text("/sys/devices/system/cpu/possible", possible, sizeof possible) > 0);
    CHECK(read_text("/sys/devices/system/cpu/present", present, sizeof present) > 0);
    CHECK(read_text("/sys/devices/system/cpu/online", online, sizeof online) > 0);
    CHECK(read_text("/sys/devices/system/cpu/kernel_max", kmax, sizeof kmax) > 0);
    CHECK(possible[0] == '0' && !strcmp(possible, present) && !strcmp(possible, online));
    CHECK(read_text("/proc/self/cgroup", cgroup, sizeof cgroup) > 0 && !strncmp(cgroup, "0::/", 4));
    CHECK(access("/sys/devices/system/cpu/possible", R_OK) == 0);
    long conf = sysconf(_SC_NPROCESSORS_CONF);
    printf("sysfs: possible=%.*s online=%.*s kernel_max=%.*s nprocs_conf=%ld\n",
           (int)strcspn(possible, "\n"), possible, (int)strcspn(online, "\n"), online,
           (int)strcspn(kmax, "\n"), kmax, conf);
}

static void tcp_info(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0);
    unsigned char info[248]; memset(info, 0xee, sizeof info);
    socklen_t len = sizeof info;
    CHECK(getsockopt(s, IPPROTO_TCP, TCP_INFO, info, &len) == 0 && len == 248);
    CHECK(info[0] == 7); /* TCP_CLOSE */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(39123); addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&addr, sizeof addr) == 0 && listen(s, 4) == 0) {
        len = 8;
        CHECK(getsockopt(s, IPPROTO_TCP, TCP_INFO, info, &len) == 0 && len == 8 && info[0] == 10);
    }
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    len = sizeof info;
    CHECK(getsockopt(u, IPPROTO_TCP, TCP_INFO, info, &len) == -1 && errno == EOPNOTSUPP);
    len = sizeof info;
    CHECK(getsockopt(s, IPPROTO_TCP, 0x7fff, info, &len) == -1 && errno == ENOPROTOOPT);
    close(s); close(u);
    printf("tcp_info: ok\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    long rseq = syscall(334, 0, 0, 0, 0); /* Brook: ENOSYS (quiet); Linux: EINVAL/EBUSY */
    printf("rseq: %ld errno=%d\n", rseq, rseq ? errno : 0);
    itimers();
    kcmps();
    fork_advice();
    sysfs();
    tcp_info();
    if (failures) { printf("COMPAT_BATCH1_FAIL %d\n", failures); return 1; }
    puts("COMPAT_BATCH1_PASS itimer/kcmp/madvise-fork/sysfs-cgroup/tcp_info");
    return 0;
}

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int failures;

static void fatal(const char *what) { perror(what); exit(1); }
static void *alloc(size_t bytes)
{
    void *p = calloc(1, bytes);
    if (!p) fatal("calloc");
    return p;
}
static void reap(pid_t pid, const char *label)
{
    int status;
    pid_t got;
    do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    if (got != pid) fatal("waitpid");
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("EXECCHECK %s %s status=0x%x\n", ok ? "PASS" : "FAIL", label, status);
    failures += !ok;
}
static void run(const char *label, const char *file, char **args, char **env, int error)
{
    pid_t pid = fork();
    if (pid < 0) fatal("fork");
    if (pid == 0) {
        // Raw syscall permits deliberately invalid pointer-vector fixtures.
        syscall(SYS_execve, file, args, env);
        int actual = errno;
        if (actual != error || !error)
            fprintf(stderr, "%s: exec errno=%d expected=%d\n", label, actual, error);
        _exit(error && actual == error ? 0 : 1);
    }
    reap(pid, label);
}
static void value(char *dst, size_t bytes, int i, int env)
{
    int prefix = snprintf(dst, bytes, env ? "EXEC_PROBE_%03d=" : "ARG_%03d:", i);
    memset(dst + prefix, 'a' + i % 26, bytes - (size_t)prefix - 1);
    dst[bytes - 1] = 0;
}
static int check_values(int argc, char **argv)
{
    if (argc < 5) return 90;
    int count = atoi(argv[2]), envcount = atoi(argv[3]), bytes = atoi(argv[4]);
    if (bytes < 32 || bytes > 256 || argc != count + 5) return 91;
    char expected[256];
    for (int i = 0; i < count; ++i) {
        value(expected, (size_t)bytes, i, 0);
        if (strcmp(argv[i + 5], expected)) return 92;
    }
    int found = 0;
    while (environ[found]) ++found;
    if (found != envcount) return 93;
    for (int i = 0; i < envcount; ++i) {
        value(expected, (size_t)bytes, i, 1);
        if (strcmp(environ[i], expected)) return 94;
    }
    return 0;
}
static void roundtrip(const char *file, int count, int envcount, int bytes, const char *label)
{
    char **args = alloc((size_t)(count + 6) * sizeof(*args));
    char **env = alloc((size_t)(envcount + 1) * sizeof(*env));
    char n[16], e[16], b[16];
    snprintf(n, sizeof(n), "%d", count);
    snprintf(e, sizeof(e), "%d", envcount);
    snprintf(b, sizeof(b), "%d", bytes);
    args[0] = (char *)file; args[1] = "--check"; args[2] = n; args[3] = e; args[4] = b;
    for (int i = 0; i < count; ++i) {
        args[i + 5] = alloc((size_t)bytes); value(args[i + 5], (size_t)bytes, i, 0);
    }
    for (int i = 0; i < envcount; ++i) {
        env[i] = alloc((size_t)bytes); value(env[i], (size_t)bytes, i, 1);
    }
    run(label, file, args, env, 0);
    for (int i = 0; i < count; ++i) free(args[i + 5]);
    for (int i = 0; i < envcount; ++i) free(env[i]);
    free(args); free(env);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--script")) { argc -= 2; argv += 2; }
    if (argc > 1 && !strcmp(argv[1], "--check")) return check_values(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--unexpected")) return 95;
    if (argc > 1 && !strcmp(argv[1], "--edge"))
        return argc == 3 && !strcmp(argv[2], "EDGE=okay") &&
               getenv("EDGE") && !strcmp(getenv("EDGE"), "okay") ? 0 : 96;
    if (argc > 1 && !strcmp(argv[1], "--long")) {
        if (argc != 3 || strlen(argv[2]) != 20000) return 97;
        for (int i = 0; i < 20000; ++i) if (argv[2][i] != 'L') return 98;
        return 0;
    }
    if (argv[0][0] != '/' || argc > 2) {
        fprintf(stderr, "usage: /absolute/path/execcheck [writable-temp-directory]\n");
        return 2;
    }
    const char *self = argv[0];
    char *empty[] = {NULL};
    char *simple[] = {(char *)self, "--unexpected", NULL};
    roundtrip(self, 128, 128, 192, "128 arguments + 128 env; >16KiB each");
    roundtrip(self, 256, 128, 192, "256 arguments; >48KiB argv strings");
    roundtrip(self, 507, 256, 64, "exactly 512 argv and 256 env entries");

    char *longarg = alloc(20001); memset(longarg, 'L', 20000);
    char *longargs[] = {(char *)self, "--long", longarg, NULL};
    run("single 20000-byte argument", self, longargs, empty, 0);
    free(longarg);

    char **many = alloc(514 * sizeof(*many));
    for (int i = 0; i < 513; ++i) many[i] = "x";
    many[0] = (char *)self; many[1] = "--unexpected";
    run("argv count E2BIG", self, many, empty, E2BIG);
    char **envmany = alloc(258 * sizeof(*envmany));
    for (int i = 0; i < 257; ++i) envmany[i] = "X=1";
    run("env count E2BIG", self, simple, envmany, E2BIG);
    free(envmany);

    char bytes[1024]; memset(bytes, 'x', sizeof(bytes) - 1); bytes[1023] = 0;
    char *bigargs[67] = {(char *)self, "--unexpected"};
    for (int i = 2; i < 66; ++i) bigargs[i] = bytes;
    bigargs[66] = NULL;
    run("aggregate argv bytes E2BIG", self, bigargs, empty, E2BIG);
    char *bigenv[34];
    for (int i = 0; i < 33; ++i) bigenv[i] = bytes;
    bigenv[33] = NULL;
    run("aggregate env bytes E2BIG", self, simple, bigenv, E2BIG);

    char *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) fatal("mmap");
    if (munmap(pages + 4096, 4096)) fatal("munmap");
    // A pointer slot itself straddles an unmapped page.
    run("argv pointer-slot EFAULT", self, (char **)(pages + 4092), empty, EFAULT);
    run("env pointer-slot EFAULT", self, simple, (char **)(pages + 4092), EFAULT);
    // The first slot is readable, but the required NULL terminator is not.
    memcpy(pages + 4088, &self, sizeof(self));
    run("missing argv terminator EFAULT", self, (char **)(pages + 4088), empty, EFAULT);
    memset(pages + 4088, 'x', 8);
    char *badargs[] = {(char *)self, "--unexpected", pages + 4088, NULL};
    char *badenv[] = {pages + 4088, NULL};
    run("argv string-page EFAULT", self, badargs, empty, EFAULT);
    run("env string-page EFAULT", self, simple, badenv, EFAULT);
    memcpy(pages + 4086, "EDGE=okay", 10);
    char *edgeargs[] = {(char *)self, "--edge", pages + 4086, NULL};
    char *edgeenv[] = {pages + 4086, NULL};
    run("NUL at page boundary", self, edgeargs, edgeenv, 0);
    munmap(pages, 4096);

    char script[512];
    snprintf(script, sizeof(script), "%s/execcheck-XXXXXX", argc == 2 ? argv[1] : "/tmp");
    int fd = mkstemp(script);
    if (fd < 0) fatal("mkstemp");
    char body[512];
    int length = snprintf(body, sizeof(body), "#!%s --script\n# Padding keeps this script above the existing 64-byte loader minimum.\n", self);
    if (length < 0 || (size_t)length >= sizeof(body)) return 1;
    if (write(fd, body, (size_t)length) != length || fchmod(fd, 0700)) fatal("script");
    close(fd);
    roundtrip(script, 128, 128, 192, "shebang preserves argv and env");
    // A valid 512-entry input no longer fits after interpreter insertion.
    many[512] = NULL;
    run("shebang expansion E2BIG", script, many, empty, E2BIG);
    unlink(script); free(many);

    printf("EXECCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

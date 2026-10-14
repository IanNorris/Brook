#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void fatal(const char *label) { perror(label); exit(2); }
static void check(int ok, const char *label)
{
    printf("STDFDCHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static double now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) fatal("clock_gettime");
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}
static int wait_child(pid_t child)
{
    double deadline = now() + 30;
    int status;
    for (;;) {
        pid_t got = waitpid(child, &status, WNOHANG);
        if (got == child) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (got < 0 && errno != EINTR) return -1;
        if (now() > deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        usleep(10000);
    }
}
static int not_tty(int fd)
{
    struct termios term;
    struct winsize size;
    errno = 0;
    if (ioctl(fd, TCGETS, &term) != -1 || errno != ENOTTY) return 0;
    errno = 0;
    return ioctl(fd, TIOCGWINSZ, &size) == -1 && errno == ENOTTY;
}
static int descriptor_cases(int target, int source)
{
    int bad = 0;
    struct stat expected, actual;
    struct termios term;
    if (fstat(source, &expected) || dup2(source, target) != target) return 255;
    if (fstat(target, &actual) || !S_ISREG(actual.st_mode) ||
        actual.st_mode != expected.st_mode || actual.st_ino != expected.st_ino ||
        actual.st_size != expected.st_size) bad |= 1;
    if (!not_tty(target)) bad |= 2;
    close(target);
    errno = 0;
    if (fstat(target, &actual) != -1 || errno != EBADF) bad |= 4;
    errno = 0;
    if (ioctl(target, TCGETS, &term) != -1 || errno != EBADF) bad |= 8;

    int pair[2];
    if (pipe(pair)) return 255;
    if (dup2(pair[0], target) != target || fstat(target, &actual) ||
        !S_ISFIFO(actual.st_mode) || !not_tty(target)) bad |= 16;
    if (pair[0] != target) close(pair[0]);
    if (pair[1] != target) close(pair[1]);
    close(target);
    int dev = open("/dev/null", O_RDWR);
    if (dev < 0 || dup2(dev, target) != target || fstat(target, &actual) ||
        !S_ISCHR(actual.st_mode) || !not_tty(target)) bad |= 32;
    if (dev >= 0 && dev != target) close(dev);
    close(target);
    return bad;
}
static void put_all(int fd, const char *s, size_t count)
{
    while (count) {
        ssize_t n = write(fd, s, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal("write fixture");
        s += n;
        count -= (size_t)n;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/') {
        fprintf(stderr, "usage: stdfdcheck absolute-writable-directory absolute-python\n");
        return 2;
    }
    char dir[256], script[320], output[320], errors[320];
    if (snprintf(dir, sizeof(dir), "%s/stdfdcheck-XXXXXX", argv[1]) >= (int)sizeof(dir)) return 2;
    if (!mkdtemp(dir)) fatal("mkdtemp");
    snprintf(script, sizeof(script), "%s/input.py", dir);
    snprintf(output, sizeof(output), "%s/stdout", dir);
    snprintf(errors, sizeof(errors), "%s/stderr", dir);
    int source = open(script, O_CREAT|O_EXCL|O_RDWR, 0640);
    if (source < 3) fatal("open script (requires initial stdio)");
    const char padding[] = "# regular-file stdin regression padding\n";
    for (int i = 0; i < 1024; ++i) put_all(source, padding, sizeof(padding)-1);
    const char program[] =
        "import os, stat\n"
        "assert stat.S_ISREG(os.fstat(0).st_mode)\n"
        "assert not os.isatty(0)\n"
        "if True:\n"
        "    result = sum(range(100))\n"
        "assert result == 4950\n"
        "print('STDFDCHECK PYTHON SCRIPT 4950')\n"
        "raise SystemExit(37)\n";
    put_all(source, program, sizeof(program)-1);
    if (lseek(source, 0, SEEK_SET)) fatal("rewind script");

    const char *labels[] = {"regular-file metadata", "regular-file ENOTTY",
        "closed descriptor fstat EBADF", "closed descriptor ioctl EBADF",
        "anonymous pipe FIFO and ENOTTY", "null device character and ENOTTY"};
    for (int target = 0; target < 3; ++target) {
        fflush(NULL);
        pid_t child = fork();
        if (child < 0) fatal("fork descriptor checks");
        if (!child) _exit(descriptor_cases(target, source));
        int result = wait_child(child);
        for (int bit = 0; bit < 6; ++bit) {
            char label[128];
            snprintf(label, sizeof(label), "fd%d %s", target, labels[bit]);
            check(result >= 0 && !(result & (1 << bit)), label);
        }
    }
    int out = open(output, O_CREAT|O_EXCL|O_RDWR, 0600);
    int err = open(errors, O_CREAT|O_EXCL|O_RDWR, 0600);
    if (out < 0 || err < 0) fatal("open captures");
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) fatal("fork Python");
    if (!child) {
        if (dup2(source, 0) != 0 || dup2(out, 1) != 1 || dup2(err, 2) != 2) _exit(90);
        close(source); close(out); close(err);
        execl(argv[2], argv[2], "-", (char *)NULL);
        _exit(91);
    }
    check(wait_child(child) == 37, "Python executes regular-file stdin with exact exit status");
    char buf[128] = {0};
    const char expected[] = "STDFDCHECK PYTHON SCRIPT 4950\n";
    ssize_t count = pread(out, buf, sizeof(buf), 0);
    struct stat st;
    check(count == sizeof(expected)-1 && !memcmp(buf, expected, sizeof(expected)-1) &&
          fstat(err, &st) == 0 && st.st_size == 0,
          "Python emits exact script output and no REPL prompts/errors");
    close(source); close(out); close(err);
    if (unlink(script) || unlink(output) || unlink(errors) || rmdir(dir)) fatal("cleanup");
    printf("STDFDCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

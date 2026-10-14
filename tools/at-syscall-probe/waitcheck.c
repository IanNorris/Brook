#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static int reap(pid_t pid)
{
    int status = 0;
    pid_t got;
    do {
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    if (got != pid) {
        perror("waitpid");
        exit(1);
    }
    return status;
}

static pid_t spawn(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    return pid;
}

static void check_exit(const char *label, int status, int expected)
{
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == expected;
    printf("WAITCHECK %s %s expected_exit=%d wait_status=0x%x\n",
           ok ? "PASS" : "FAIL", label, expected, status);
    failures += !ok;
}

int main(int argc, char **argv)
{
    // Invoked as an external command by the Bash set -e checks below.
    if (argc == 3 && strcmp(argv[1], "--exit") == 0)
        _exit(atoi(argv[2]));
    if (argc != 2) {
        fprintf(stderr, "usage: %s /absolute/path/to/bash\n", argv[0]);
        return 2;
    }

    const int codes[] = {0, 1, 127, 128, 255};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        // Test the actual Linux exit and exit_group entry points separately.
        for (int group = 0; group < 2; ++group) {
            pid_t pid = spawn();
            if (pid == 0) {
                syscall(group ? SYS_exit_group : SYS_exit, codes[i]);
                _exit(99);
            }
            check_exit(group ? "exit_group" : "exit", reap(pid), codes[i]);
        }
    }

    const int signals[] = {SIGTERM, SIGKILL};
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        pid_t pid = spawn();
        if (pid == 0) {
            for (;;) pause();
        }
        if (kill(pid, signals[i]) != 0) {
            perror("kill");
            return 1;
        }
        int status = reap(pid);
        int ok = WIFSIGNALED(status) && WTERMSIG(status) == signals[i];
        printf("WAITCHECK %s signal=%d wait_status=0x%x\n",
               ok ? "PASS" : "FAIL", signals[i], status);
        failures += !ok;
    }

    // Run this binary as an external child of Bash: a builtin exit alone
    // would not exercise Bash decoding a child's wait4 result. If exit128
    // is misreported as success, set -e continues and incorrectly returns77.
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        char code[16];
        snprintf(code, sizeof(code), "%d", codes[i]);
        pid_t pid = spawn();
        if (pid == 0) {
            execl(argv[1], argv[1], "--noprofile", "--norc", "-c",
                  "set -e; \"$1\" --exit \"$2\"; exit 77",
                  "waitcheck", argv[0], code, (char *)NULL);
            perror("exec bash");
            _exit(99);
        }
        check_exit("bash-set-e", reap(pid), codes[i] ? codes[i] : 77);
    }

    printf("WAITCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

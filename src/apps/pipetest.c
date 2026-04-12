// pipetest.c — Test pipe() + dup2() + fork/exec pipeline.
// Creates a pipe, forks, child writes to pipe, parent reads from pipe.
// Also tests dup2 by redirecting stdout through a pipe.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

int main(void)
{
    printf("=== PIPE TEST ===\n");

    // Test 1: Basic pipe read/write
    int pfd[2];
    if (pipe(pfd) < 0)
    {
        printf("FAIL: pipe() failed\n");
        return 1;
    }
    printf("pipe fds: read=%d write=%d\n", pfd[0], pfd[1]);

    const char* msg = "Hello through pipe!";
    ssize_t written = write(pfd[1], msg, strlen(msg));
    printf("wrote %zd bytes to pipe\n", written);

    char buf[64] = {};
    ssize_t rd = read(pfd[0], buf, sizeof(buf) - 1);
    buf[rd] = '\0';
    printf("read %zd bytes from pipe: '%s'\n", rd, buf);

    if (rd != written || strcmp(buf, msg) != 0)
    {
        printf("FAIL: pipe data mismatch\n");
        close(pfd[0]);
        close(pfd[1]);
        return 1;
    }
    printf("Test 1 PASSED: basic pipe\n");

    close(pfd[0]);
    close(pfd[1]);

    // Test 2: Fork + pipe (parent reads from child)
    int pfd2[2];
    if (pipe(pfd2) < 0)
    {
        printf("FAIL: pipe() #2 failed\n");
        return 1;
    }

    pid_t child = fork();
    if (child < 0)
    {
        printf("FAIL: fork() failed\n");
        return 1;
    }

    if (child == 0)
    {
        // Child: close read end, write message, exit
        close(pfd2[0]);
        const char* childMsg = "from child!";
        write(pfd2[1], childMsg, strlen(childMsg));
        close(pfd2[1]);
        _exit(0);
    }

    // Parent: close write end, read from pipe
    close(pfd2[1]);
    char buf2[64] = {};
    ssize_t rd2 = read(pfd2[0], buf2, sizeof(buf2) - 1);
    buf2[rd2 > 0 ? rd2 : 0] = '\0';
    close(pfd2[0]);

    int status = 0;
    waitpid(child, &status, 0);

    printf("read from child pipe: '%s' (%zd bytes)\n", buf2, rd2);
    if (rd2 > 0 && strcmp(buf2, "from child!") == 0)
        printf("Test 2 PASSED: fork+pipe\n");
    else
    {
        printf("FAIL: fork+pipe data mismatch\n");
        return 1;
    }

    // Test 3: dup2
    int pfd3[2];
    if (pipe(pfd3) < 0)
    {
        printf("FAIL: pipe() #3 failed\n");
        return 1;
    }

    int saved_stdout = dup(STDOUT_FILENO);
    dup2(pfd3[1], STDOUT_FILENO);
    close(pfd3[1]);

    // This printf goes to the pipe now
    printf("redirected!");
    // Flush - musl should flush on write
    fflush(stdout);

    // Restore stdout
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    char buf3[64] = {};
    ssize_t rd3 = read(pfd3[0], buf3, sizeof(buf3) - 1);
    buf3[rd3 > 0 ? rd3 : 0] = '\0';
    close(pfd3[0]);

    printf("dup2 redirect captured: '%s'\n", buf3);
    if (rd3 > 0 && strcmp(buf3, "redirected!") == 0)
        printf("Test 3 PASSED: dup2\n");
    else
    {
        printf("FAIL: dup2 redirect mismatch (got %zd bytes)\n", rd3);
        return 1;
    }

    printf("=== ALL PIPE TESTS PASSED ===\n");
    return 0;
}

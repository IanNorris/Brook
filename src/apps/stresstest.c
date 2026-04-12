// stresstest.c — Multi-process stress test for Brook OS
// Tests fork+exec+pipe+dup2 in combination under load.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define NUM_WORKERS 4
#define ITERATIONS  10

// Worker: compute fibonacci, write result to pipe
static void worker(int id, int write_fd)
{
    long fib_a = 0, fib_b = 1;
    for (int i = 0; i < 30 + id; i++)
    {
        long tmp = fib_a + fib_b;
        fib_a = fib_b;
        fib_b = tmp;
    }

    char buf[64];
    int len = 0;
    // Simple itoa
    long val = fib_b;
    char digits[20];
    int di = 0;
    if (val == 0) digits[di++] = '0';
    while (val > 0) { digits[di++] = '0' + (val % 10); val /= 10; }
    for (int i = di - 1; i >= 0; i--) buf[len++] = digits[i];
    buf[len++] = '\n';

    write(write_fd, buf, len);
    close(write_fd);
}

int main(void)
{
    printf("=== STRESS TEST: %d workers x %d iterations ===\n",
           NUM_WORKERS, ITERATIONS);

    int total_ok = 0, total_fail = 0;

    for (int iter = 0; iter < ITERATIONS; iter++)
    {
        int pipes[NUM_WORKERS][2];
        pid_t pids[NUM_WORKERS];

        // Fork workers, each writes fibonacci result to its pipe
        for (int i = 0; i < NUM_WORKERS; i++)
        {
            if (pipe(pipes[i]) < 0) { total_fail++; continue; }

            pid_t child = fork();
            if (child < 0) { total_fail++; continue; }
            if (child == 0)
            {
                close(pipes[i][0]);
                worker(i, pipes[i][1]);
                _exit(0);
            }
            pids[i] = child;
            close(pipes[i][1]);
        }

        // Collect results
        for (int i = 0; i < NUM_WORKERS; i++)
        {
            char buf[64] = {};
            int n = read(pipes[i][0], buf, sizeof(buf) - 1);
            close(pipes[i][0]);

            int status = 0;
            waitpid(pids[i], &status, 0);

            if (n > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                total_ok++;
            else
                total_fail++;
        }
    }

    printf("Results: %d OK, %d FAIL (of %d total)\n",
           total_ok, total_fail, NUM_WORKERS * ITERATIONS);
    printf("=== STRESS TEST %s ===\n",
           total_fail == 0 ? "PASSED" : "FAILED");
    return total_fail > 0 ? 1 : 0;
}

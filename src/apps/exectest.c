// exectest.c — Test fork() + execve() syscalls.
// Forks a child that exec's the "hello" binary. Parent waits for child.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

int main(int argc, char** argv)
{
    printf("=== EXEC TEST ===\n");
    printf("Parent PID: %d\n", getpid());

    pid_t child = fork();
    if (child < 0)
    {
        printf("FAIL: fork() returned %d\n", child);
        return 1;
    }

    if (child == 0)
    {
        // Child process — exec the hello binary
        printf("Child PID %d: about to exec /boot/BIN/HELLO\n", getpid());
        char* args[] = { "/boot/BIN/HELLO", "from_exec", NULL };
        char* env[] = { "HOME=/", NULL };
        execve("/boot/BIN/HELLO", args, env);

        // If execve returns, it failed
        printf("FAIL: execve returned (errno implied)\n");
        _exit(99);
    }

    // Parent — wait for child
    printf("Parent: waiting for child %d\n", child);
    int status = 0;
    pid_t waited = waitpid(child, &status, 0);

    if (waited != child)
    {
        printf("FAIL: waitpid returned %d, expected %d\n", waited, child);
        return 1;
    }

    if (WIFEXITED(status))
    {
        int exitCode = WEXITSTATUS(status);
        printf("Parent: child %d exited with status %d\n", child, exitCode);
        if (exitCode == 0)
            printf("=== EXEC TEST PASSED ===\n");
        else
            printf("=== EXEC TEST FAILED (child exit %d) ===\n", exitCode);
        return exitCode;
    }

    printf("FAIL: child did not exit normally\n");
    return 1;
}

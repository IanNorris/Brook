// bbtest.c — Test busybox on Brook OS
// Fork+exec busybox with proper argv[0] to work around FAT32 uppercase

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int run_busybox(const char* applet, char* const argv[])
{
    pid_t child = fork();
    if (child < 0) { printf("fork failed\n"); return -1; }
    if (child == 0)
    {
        execve("/boot/BIN/BUSYBOX", argv, NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void)
{
    printf("=== BUSYBOX TEST ===\n");

    // Test 1: busybox echo
    printf("Test 1: busybox echo\n");
    {
        char* argv[] = { "busybox", "echo", "Hello from busybox!", NULL };
        int rc = run_busybox("echo", argv);
        printf("  exit code: %d\n", rc);
    }

    // Test 2: busybox uname
    printf("Test 2: busybox uname\n");
    {
        char* argv[] = { "busybox", "uname", "-a", NULL };
        int rc = run_busybox("uname", argv);
        printf("  exit code: %d\n", rc);
    }

    // Test 3: busybox cat (via pipe)
    printf("Test 3: busybox cat via pipe\n");
    {
        int pfd[2];
        if (pipe(pfd) < 0) { printf("  pipe failed\n"); return 1; }

        pid_t child = fork();
        if (child < 0) { printf("  fork failed\n"); return 1; }

        if (child == 0)
        {
            // Child: redirect stdin from pipe, exec busybox cat
            close(pfd[1]);
            dup2(pfd[0], 0);
            close(pfd[0]);
            char* argv[] = { "cat", NULL };
            execve("/boot/BIN/BUSYBOX", argv, NULL);
            _exit(127);
        }

        // Parent: write to pipe, close write end, wait
        close(pfd[0]);
        const char* msg = "piped input works!\n";
        write(pfd[1], msg, 19);
        close(pfd[1]);

        int status = 0;
        waitpid(child, &status, 0);
        printf("  exit code: %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }

    // Test 4: busybox ash -c 'echo shell works'
    printf("Test 4: busybox ash\n");
    {
        char* argv[] = { "ash", "-c", "echo shell works; exit 0", NULL };
        int rc = run_busybox("ash", argv);
        printf("  exit code: %d\n", rc);
    }

    printf("=== BUSYBOX TEST DONE ===\n");
    return 0;
}

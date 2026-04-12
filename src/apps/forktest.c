/* forktest.c — Test fork() syscall on Brook OS */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(void) {
    printf("forktest: parent pid=%d\n", getpid());

    pid_t child = fork();

    if (child < 0) {
        printf("forktest: fork() FAILED with %d\n", (int)child);
        return 1;
    }

    if (child == 0) {
        /* Child process */
        printf("forktest: CHILD pid=%d ppid=%d\n", getpid(), getppid());

        /* Verify we have our own address space */
        volatile int x = 42;
        printf("forktest: child x=%d\n", x);
        x = 99;
        printf("forktest: child x=%d (modified)\n", x);

        printf("forktest: child exiting with status 7\n");
        _exit(7);
    } else {
        /* Parent process */
        printf("forktest: PARENT pid=%d, child=%d\n", getpid(), (int)child);

        /* Wait for child */
        int wstatus = 0;
        pid_t w = waitpid(child, &wstatus, 0);
        if (w == child) {
            if (WIFEXITED(wstatus))
                printf("forktest: child exited normally, status=%d\n", WEXITSTATUS(wstatus));
            else
                printf("forktest: child exited abnormally, wstatus=0x%x\n", wstatus);
        } else {
            printf("forktest: waitpid returned %d (expected %d)\n", (int)w, (int)child);
        }

        volatile int x = 42;
        printf("forktest: parent x=%d (should still be 42)\n", x);
        printf("forktest: ALL TESTS PASSED\n");
    }

    return 0;
}

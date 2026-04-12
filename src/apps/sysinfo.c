// sysinfo.c — System information display for Brook OS.
// Validates: uname, getpid, getuid, clock_gettime.

#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <time.h>

int main(void) {
    struct utsname uts;
    if (uname(&uts) == 0) {
        printf("System:  %s %s %s\n", uts.sysname, uts.release, uts.machine);
        printf("Node:    %s\n", uts.nodename);
    }
    printf("PID:     %d\n", getpid());
    printf("UID:     %d\n", getuid());

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("Uptime:  %ld.%03ld s\n", ts.tv_sec, ts.tv_nsec / 1000000);

    return 0;
}

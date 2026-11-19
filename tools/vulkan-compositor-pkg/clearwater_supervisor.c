#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int wait_event(const sigset_t *events, const struct timespec *timeout)
{
    int signal_number;
    do {
        signal_number = sigtimedwait(events, NULL, timeout);
    } while (signal_number < 0 && errno == EINTR);
    return signal_number;
}

static int wait_child(pid_t child, int *status, int options)
{
    pid_t result;
    do {
        result = waitpid(child, status, options);
    } while (result < 0 && errno == EINTR);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: CWSUP.ELF /absolute/path/to/CWSHELL.ELF\n");
        return 2;
    }

    /* Consume signals synchronously so termination cannot race a blocking
       wait or arrive between fork and recording the child PID. */
    sigset_t events, original_mask;
    sigemptyset(&events);
    sigaddset(&events, SIGTERM);
    sigaddset(&events, SIGINT);
    sigaddset(&events, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &events, &original_mask) < 0) {
        perror("CLEARWATER_SUPERVISOR sigprocmask");
        return 1;
    }

    struct sigaction child_action = {.sa_handler = SIG_DFL, .sa_flags = SA_NOCLDSTOP};
    sigemptyset(&child_action.sa_mask);
    if (sigaction(SIGCHLD, &child_action, NULL) < 0) {
        perror("CLEARWATER_SUPERVISOR sigaction");
        return 1;
    }

    unsigned delay = 1;
    for (;;) {
        struct timespec started, finished;
        int have_clock = clock_gettime(CLOCK_MONOTONIC, &started) == 0;
        pid_t child = fork();
        if (child == 0) {
            if (sigprocmask(SIG_SETMASK, &original_mask, NULL) == 0)
                execl(argv[1], argv[1], (char *)NULL);
            perror("CLEARWATER_SUPERVISOR exec");
            _exit(127);
        }

        if (child < 0) {
            perror("CLEARWATER_SUPERVISOR fork");
        } else {
            fprintf(stderr, "CLEARWATER_SUPERVISOR_START pid=%ld\n", (long)child);
            int status;
            int result = 0;
            int event;
            do {
                event = wait_event(&events, NULL);
                if (event == SIGCHLD)
                    result = wait_child(child, &status, WNOHANG);
            } while (event == SIGCHLD && result == 0);
            if (result < 0) {
                perror("CLEARWATER_SUPERVISOR waitpid");
                return 1;
            }
            if (event != SIGCHLD) {
                if (event < 0)
                    perror("CLEARWATER_SUPERVISOR sigtimedwait");
                (void)kill(child, SIGTERM);
                if (wait_event(&events, &(struct timespec){5, 0}) == SIGCHLD &&
                    wait_child(child, &status, WNOHANG) == child)
                    return event < 0;
                (void)kill(child, SIGKILL);
                return wait_child(child, &status, 0) < 0 || event < 0;
            }

            if (have_clock && clock_gettime(CLOCK_MONOTONIC, &finished) == 0 &&
                finished.tv_sec - started.tv_sec >= 30)
                delay = 1;
            fprintf(stderr, "CLEARWATER_SUPERVISOR_EXIT status=%d retry_s=%u\n", status, delay);
        }

        int event = wait_event(&events, &(struct timespec){delay, 0});
        if (event == SIGTERM || event == SIGINT)
            return 0;
        if (event < 0 && errno != EAGAIN) {
            perror("CLEARWATER_SUPERVISOR sigtimedwait");
            return 1;
        }
        if (delay < 30)
            delay = delay > 15 ? 30 : delay * 2;
    }
}

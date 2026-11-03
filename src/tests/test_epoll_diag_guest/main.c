// Exercise the opt-in epoll diagnostics without printing pointer values.
#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

int main(void)
{
    int ep = epoll_create1(0);
    int fd = eventfd(0, EFD_NONBLOCK);
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 1};
    if (ep < 0 || fd < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event)) return 1;
    errno = 0;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) != -1 || errno != EEXIST) return 2;
    if (close(fd)) return 3;
    int pipefd[2];
    if (pipe(pipefd) || pipefd[0] != fd) return 4;
    errno = 0;
    // Report the kernel's behavior; either result lets the host verify that
    // the diagnostic counters agree. The compatibility probe separately
    // requires Linux's success result.
    int replaced = epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &event);
    if (replaced != 0 && (replaced != -1 || errno != EEXIST)) return 5;
    printf("EPOLL_REPLACED_ADD=%s\n", replaced == 0 ? "ok" : "exists");
    puts("EPOLL_DIAG_GUEST_REPRODUCED");
    close(pipefd[0]); close(pipefd[1]); close(ep);
    return 0;
}

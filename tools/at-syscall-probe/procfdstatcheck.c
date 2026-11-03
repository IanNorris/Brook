#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the follow-stat operation used when enumerating /proc/self/fd.
 * Keep every source descriptor live while comparing file type and memfd size;
 * this does not test descriptor close/reuse races or O_PATH lifetime. */
static unsigned failures;
static void check_fd(int procfd, int fd, const char *name)
{
    char number[32];
    struct stat direct, followed, link;
    snprintf(number, sizeof(number), "%d", fd);
    if (fstat(fd, &direct)) {
        printf("FAIL %s fstat errno=%d\n", name, errno);
        ++failures;
        return;
    }
    if (fstatat(procfd, number, &followed, 0)) {
        printf("FAIL %s follow errno=%d\n", name, errno);
        ++failures;
        return;
    }
    if (fstatat(procfd, number, &link, AT_SYMLINK_NOFOLLOW)) {
        printf("FAIL %s nofollow errno=%d\n", name, errno);
        ++failures;
        return;
    }
    if (!S_ISLNK(link.st_mode) || (direct.st_mode & S_IFMT) != (followed.st_mode & S_IFMT) ||
        (S_ISREG(direct.st_mode) && direct.st_size != followed.st_size)) {
        printf("FAIL %s mismatch\n", name);
        ++failures;
        return;
    }
    printf("PASS %s\n", name);
}

int main(void)
{
    int procfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int dirfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int memfd = memfd_create("brook-procfd-check", MFD_CLOEXEC);
    int fds[2], sockets[2];
    int event = eventfd(0, EFD_CLOEXEC);
    if (procfd < 0 || dirfd < 0 || memfd < 0 || event < 0 || pipe(fds) ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) || ftruncate(memfd, 8193)) {
        perror("fixture");
        return 2;
    }
    check_fd(procfd, dirfd, "directory");
    check_fd(procfd, memfd, "memfd");
    check_fd(procfd, fds[0], "pipe-read");
    check_fd(procfd, fds[1], "pipe-write");
    check_fd(procfd, sockets[0], "socket");
    check_fd(procfd, event, "eventfd");
    close(event); close(sockets[0]); close(sockets[1]); close(fds[0]); close(fds[1]);
    close(memfd); close(dirfd); close(procfd);
    return failures ? 1 : 0;
}

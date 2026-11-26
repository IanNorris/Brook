#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define GET_DESC_SIZE 0x80044801UL
#define GET_DESC 0x90044802UL
#define GET_INFO 0x80084803UL

static const char node[] = "/dev/hidraw0";

static void check(int ok, const char *step)
{
    if (!ok) {
        dprintf(2, "FIDO_TEST_FAIL %s errno=%d\n", step, errno);
        _exit(1);
    }
}

static int await_node(int flags)
{
    for (unsigned i = 0; i < 400; ++i) {
        int fd = open(node, flags | O_CLOEXEC);
        if (fd >= 0) return fd;
        check(errno == ENOENT || errno == ENODEV, "waiting for node");
        usleep(50000);
    }
    check(0, "attach timed out");
    return -1;
}

static void exchange(int fd, uint8_t command, const uint8_t channel[4],
                     const uint8_t *data, unsigned length, uint8_t response[64])
{
    uint8_t report[65] = {0};
    memcpy(report + 1, channel, 4);
    report[5] = command;
    report[6] = length >> 8;
    report[7] = length & 255;
    memcpy(report + 8, data, length);
    check(write(fd, report, sizeof(report)) == sizeof(report), "report write");
    struct pollfd wait = {fd, POLLIN, 0};
    check(poll(&wait, 1, 5000) == 1 && wait.revents == POLLIN, "report poll");
    check(read(fd, response, 64) == 64, "report read");
    check(!memcmp(response, channel, 4) && response[4] == command, "response header");
}

static void roundtrip(int fd)
{
    uint8_t response[64];
    const uint8_t broadcast[] = {255, 255, 255, 255};
    const uint8_t nonce[] = {0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xa8};
    exchange(fd, 0x86, broadcast, nonce, sizeof(nonce), response);
    check(response[5] == 0 && response[6] == 17 && !memcmp(response + 7, nonce, sizeof(nonce)),
          "CTAPHID INIT echo");
    uint8_t channel[4];
    memcpy(channel, response + 15, sizeof(channel));
    check(memcmp(channel, broadcast, sizeof(channel)), "CTAPHID assigned channel");
    const uint8_t payload[] = {0xe1, 0x00, 0x72, 0xff, 0x43};
    exchange(fd, 0x81, channel, payload, sizeof(payload), response);
    check(response[5] == 0 && response[6] == sizeof(payload) &&
          !memcmp(response + 7, payload, sizeof(payload)), "CTAPHID PING echo");
}

int main(void)
{
    check(getuid() == 1000 && geteuid() == 1000, "desktop credentials");
    check(access(node, F_OK) == -1 && errno == ENOENT, "no node before hotplug");
    dprintf(1, "FIDO_TEST_READY\n");
    int fd = await_node(O_RDWR | O_NONBLOCK);
    struct stat path_stat, fd_stat;
    check(stat(node, &path_stat) == 0 && fstat(fd, &fd_stat) == 0 &&
          S_ISCHR(fd_stat.st_mode) && fd_stat.st_rdev == path_stat.st_rdev &&
          fd_stat.st_dev == path_stat.st_dev && fd_stat.st_ino == path_stat.st_ino &&
          path_stat.st_uid == 1000 && (path_stat.st_mode & 0777) == 0600, "device inode");

    int size = 0;
    struct { uint32_t size; uint8_t value[4096]; } descriptor = {0};
    struct { uint32_t bus; uint16_t vendor, product; } info = {0};
    check(ioctl(fd, GET_DESC_SIZE, &size) == 0 && size == 34, "descriptor size");
    descriptor.size = size;
    check(ioctl(fd, GET_DESC, &descriptor) == 0 &&
          !memcmp(descriptor.value, "\x06\xd0\xf1\x09\x01\xa1\x01", 7) &&
          descriptor.value[size - 1] == 0xc0, "descriptor bytes");
    check(ioctl(fd, GET_INFO, &info) == 0 && info.bus == 3 &&
          info.vendor == 0x46f4 && info.product == 5, "USB identity");
    uint8_t empty[64];
    check(read(fd, empty, sizeof(empty)) == -1 && errno == EAGAIN, "empty nonblocking read");
    roundtrip(fd);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event request = {.events = EPOLLIN, .data.fd = fd}, event = {0};
    check(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, fd, &request) == 0, "register epoll");
    dprintf(1, "FIDO_TEST_ROUNDTRIP\n");
    check(epoll_wait(ep, &event, 1, 15000) == 1 && event.data.fd == fd &&
          (event.events & (EPOLLHUP | EPOLLERR)) == (EPOLLHUP | EPOLLERR), "unplug epoll");
    check(read(fd, empty, sizeof(empty)) == -1 && errno == ENODEV, "unplug read");
    int exists;
    for (unsigned i = 0; (exists = access(node, F_OK)) == 0 && i < 20; ++i)
        usleep(50000);
    check(exists == -1 && errno == ENOENT, "unplug removed node");
    dprintf(1, "FIDO_TEST_UNPLUGGED\n");

    int next = await_node(O_RDWR);
    check(fstat(next, &fd_stat) == 0 && fd_stat.st_rdev != path_stat.st_rdev, "new incarnation");
    check(read(fd, empty, sizeof(empty)) == -1 && errno == ENODEV, "stale fd remains detached");
    roundtrip(next);
    close(next);
    close(fd);
    close(ep);
    dprintf(1, "FIDO_TEST_PASS uid=1000 CTAPHID_INIT_PING hotplug unplug replug\n");
    return 0;
}

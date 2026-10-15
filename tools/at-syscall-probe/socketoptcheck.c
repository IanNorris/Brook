#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do { if (!(x)) { printf("SOCKETOPTCHECK FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); ++failures; } } while (0)
static int get(int fd, int option) {
    int value = -1234567; socklen_t length = sizeof(value);
    CHECK(getsockopt(fd, SOL_SOCKET, option, &value, &length) == 0);
    CHECK(length == sizeof(value)); return value;
}
int main(int argc, char **argv)
{
    syscall(SYS_alarm, 30);
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    if (failures) return 1;
    int send = get(pair[0], SO_SNDBUF), recv = get(pair[0], SO_RCVBUF);
    CHECK(send > 0 && recv > 0);
    CHECK(get(pair[0], SO_TYPE) == SOCK_STREAM);
    CHECK(get(pair[0], SO_ERROR) == 0);
    CHECK(get(pair[1], SO_ERROR) == 0);
    for (socklen_t count = 0; count <= 8; ++count) {
        unsigned char output[8]; memset(output, 0xa5, sizeof(output));
        socklen_t length = count;
        CHECK(getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, output, &length) == 0);
        socklen_t expected = count < sizeof(send) ? count : sizeof(send);
        CHECK(length == expected);
        CHECK(memcmp(output, &send, expected) == 0);
        for (unsigned i = expected; i < sizeof(output); ++i) CHECK(output[i] == 0xa5);
    }
    socklen_t length = 0;
    CHECK(getsockopt(pair[0], SOL_SOCKET, SO_TYPE, NULL, &length) == 0 && length == 0);
    int value = 0;
    length = sizeof(value); errno = 0;
    CHECK(getsockopt(-1, SOL_SOCKET, SO_TYPE, &value, &length) == -1 && errno == EBADF);
    int regular = open("/dev/null", O_RDONLY); CHECK(regular >= 0);
    length = sizeof(value); errno = 0;
    CHECK(getsockopt(regular, SOL_SOCKET, SO_TYPE, &value, &length) == -1 && errno == ENOTSOCK);
    length = sizeof(value); errno = 0;
    CHECK(getsockopt(pair[0], SOL_SOCKET, 0x7fffffff, &value, &length) == -1 && errno == ENOPROTOOPT);
    length = sizeof(value); errno = 0;
    CHECK(getsockopt(pair[0], 0x7fffffff, SO_TYPE, &value, &length) == -1 && errno == EOPNOTSUPP);
    errno = 0;
    CHECK(syscall(SYS_getsockopt, pair[0], SOL_SOCKET, SO_TYPE, &value, (void *)1) == -1 && errno == EFAULT);
    length = sizeof(value); errno = 0;
    CHECK(syscall(SYS_getsockopt, pair[0], SOL_SOCKET, SO_TYPE, (void *)1, &length) == -1 && errno == EFAULT);
    length = (socklen_t)-1; errno = 0;
    CHECK(getsockopt(pair[0], SOL_SOCKET, SO_TYPE, &value, &length) == -1 && errno == EINVAL);
    length = sizeof(value);
    CHECK(syscall(SYS_getsockopt, ((uint64_t)0x12345678 << 32) | (uint32_t)pair[0], SOL_SOCKET, SO_TYPE, &value, &length) == 0 && value == SOCK_STREAM);
    if (argc == 2 && !strcmp(argv[1], "--brook")) {
        int unbound = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(unbound >= 0 && get(unbound, SO_TYPE) == SOCK_STREAM);
        length = sizeof(value); errno = 0;
        CHECK(getsockopt(unbound, SOL_SOCKET, SO_SNDBUF, &value, &length) == -1 && errno == ENOTCONN);
        close(unbound);
        CHECK(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
        CHECK(send > 0 && send <= 1024 * 1024);
        if (send > 0 && send <= 1024 * 1024) {
            char *payload = malloc((size_t)send + 1);
            CHECK(payload != NULL);
            if (payload) {
                memset(payload, 'P', (size_t)send + 1);
                CHECK(write(pair[0], payload, (size_t)send + 1) == send);
                errno = 0; CHECK(write(pair[0], payload, 1) == -1 && errno == EAGAIN);
                CHECK(read(pair[1], payload, (size_t)send) == send);
                free(payload);
            }
        }
    }
    close(regular); close(pair[0]); close(pair[1]);
    printf("SOCKETOPTCHECK %s sndbuf=%d rcvbuf=%d failures=%d\n", failures ? "FAIL" : "PASS", send, recv, failures);
    return failures ? 1 : 0;
}

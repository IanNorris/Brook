/* AF_UNIX MSG_PEEK must leave bytes queued (Linux semantics).
 *
 * .NET implements a zero-byte socket receive as recv(fd, &b, 1, MSG_PEEK)
 * and then reads the real data with recvmsg. Tmds.DBus (Avalonia apps such
 * as Mesen) uses that pattern; a kernel that consumes the peeked byte
 * corrupts D-Bus framing and the bus connection drops. The same program
 * passes natively on Linux, which validates the expectations themselves.
 * Prints UNIX_PEEK_OK or UNIX_PEEK_FAIL count=N.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { ++failures; printf("UNIX_PEEK check failed line %d: %s (errno=%d)\n", __LINE__, #c, errno); } } while (0)

static void stream_case(int type) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, type, 0, sv) == 0);
    char b[16] = {0};
    CHECK(write(sv[0], "ABCDEF", 6) == 6);
    /* The .NET readiness probe. */
    CHECK(recv(sv[1], b, 1, MSG_PEEK) == 1 && b[0] == 'A');
    CHECK(recv(sv[1], b, 1, MSG_PEEK | MSG_DONTWAIT) == 1 && b[0] == 'A');
    memset(b, 0, sizeof b);
    if (type == SOCK_STREAM) {
        CHECK(recv(sv[1], b, 3, MSG_PEEK) == 3 && !memcmp(b, "ABC", 3));
        /* recvmsg peek with two iovecs fills from the queue head only once. */
        char x[2] = {0}, y[8] = {0};
        struct iovec iov[2] = {{x, 2}, {y, 8}};
        struct msghdr m = {.msg_iov = iov, .msg_iovlen = 2};
        ssize_t n = recvmsg(sv[1], &m, MSG_PEEK);
        CHECK(n >= 2 && !memcmp(x, "AB", 2));
        if (n > 2) CHECK(!memcmp(y, "CDEF", (size_t)(n - 2)));
        /* Tmds.DBus's real read after the probe. */
        memset(b, 0, sizeof b);
        struct iovec one = {b, sizeof b};
        struct msghdr r = {.msg_iov = &one, .msg_iovlen = 1};
        CHECK(recvmsg(sv[1], &r, 0) == 6 && !memcmp(b, "ABCDEF", 6));
    } else {
        /* A record peek returns the record and leaves it queued. */
        CHECK(recv(sv[1], b, sizeof b, MSG_PEEK) == 6 && !memcmp(b, "ABCDEF", 6));
        memset(b, 0, sizeof b);
        CHECK(recv(sv[1], b, sizeof b, 0) == 6 && !memcmp(b, "ABCDEF", 6));
    }
    /* Nothing left: a nonblocking peek reports EAGAIN, not end of stream. */
    errno = 0;
    CHECK(recv(sv[1], b, 1, MSG_PEEK | MSG_DONTWAIT) == -1 && errno == EAGAIN);
    int flags = fcntl(sv[1], F_GETFL);
    CHECK(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK) == 0);
    errno = 0;
    CHECK(recv(sv[1], b, 1, MSG_PEEK) == -1 && errno == EAGAIN);
    /* After the peer closes, a peek reports end of stream. */
    close(sv[0]);
    if (type == SOCK_STREAM) CHECK(recv(sv[1], b, 1, MSG_PEEK) == 0);
    close(sv[1]);
}

int main(void) {
    stream_case(SOCK_STREAM);
    stream_case(SOCK_SEQPACKET);
    if (failures) printf("UNIX_PEEK_FAIL count=%d\n", failures);
    else printf("UNIX_PEEK_OK\n");
    fflush(stdout);
    return failures != 0;
}

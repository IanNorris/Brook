// Headless QEMU fixture. A byte-for-byte checked TCP stream in each direction.
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { Port = 4211, BlockBytes = 65536, Blocks = 128 };
static uint8_t pattern[BlockBytes], buffer[BlockBytes];

static bool transfer(int fd, void *data, size_t size, bool sending)
{
    uint8_t *p = data;
    while (size) {
        ssize_t n = sending ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

int main(void)
{
    for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = (i * 17 + 3) % 251;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(Port)};
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
        listen(listener, 1)) {
        perror("E1000_BENCH listen");
        return 1;
    }
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        char direction;
        if (!transfer(fd, &direction, 1, false)) {
            close(fd);
            continue;
        }
        bool ok = direction == 'D' || direction == 'U';
        for (unsigned i = 0; ok && i < Blocks; ++i) {
            if (direction == 'D') ok = transfer(fd, pattern, sizeof(pattern), true);
            else ok = transfer(fd, buffer, sizeof(buffer), false) &&
                      memcmp(pattern, buffer, sizeof(pattern)) == 0;
        }
        char result = ok ? '+' : '-';
        if (direction == 'U') transfer(fd, &result, 1, true);
        close(fd);
    }
}

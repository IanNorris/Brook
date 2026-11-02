/* Send a deterministic 4 MiB payload using a requested write size. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

enum { TOTAL = 4 * 1024 * 1024 };
static uint8_t payload[TOTAL];

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    char *end;
    unsigned long port = strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || !port || port > 65535) return 2;
    unsigned long chunk = strtoul(argv[3], &end, 10);
    if (!*argv[3] || *end || chunk < 1 || chunk > 262144) return 2;
    for (size_t i = 0; i < TOTAL; ++i) payload[i] = (uint8_t)(i * 17u + 3u);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        perror("connect"); close(fd); return 1;
    }
    unsigned char ready;
    if (recv(fd, &ready, 1, 0) != 1 || ready != 0x42) { perror("ready"); close(fd); return 1; }
    size_t sent = 0, calls = 0, partials = 0;
    while (sent < TOTAL) {
        size_t want = TOTAL - sent < chunk ? TOTAL - sent : chunk;
        ssize_t n = send(fd, payload + sent, want, MSG_NOSIGNAL);
        ++calls;
        if (n <= 0 || (size_t)n > want) { perror("send"); close(fd); return 1; }
        if ((size_t)n != want) ++partials;
        sent += (size_t)n;
    }
    const unsigned char end_marker = 0x45;
    if (send(fd, &end_marker, 1, MSG_NOSIGNAL) != 1) { perror("end marker"); close(fd); return 1; }
    unsigned char ack;
    if (recv(fd, &ack, 1, 0) != 1 || ack != 0x4b) { perror("ack"); close(fd); return 1; }
    if (close(fd)) { perror("close"); return 1; }
    printf("TCP_CHUNKS_OK bytes=%zu chunk=%lu calls=%zu partials=%zu\n", sent, chunk, calls, partials);
    return 0;
}

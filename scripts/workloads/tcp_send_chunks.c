/* Send a deterministic payload using a requested write size. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

enum { DEFAULT_TOTAL = 4 * 1024 * 1024, MAX_TOTAL = 64 * 1024 * 1024 };

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5 && argc != 6 && argc != 7) return 2;
    char *end;
    unsigned long port = strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || !port || port > 65535) return 2;
    unsigned long chunk = strtoul(argv[3], &end, 10);
    if (!*argv[3] || *end || chunk < 1 || chunk > 262144) return 2;
    unsigned long vectors = 1;
    int vectored = 0;
    if (argc >= 6) {
        if (!strcmp(argv[4], "writev")) vectored = 1;
        else if (!strcmp(argv[4], "sendmsg")) vectored = 2;
        else return 2;
        vectors = strtoul(argv[5], &end, 10);
        if (!*argv[5] || *end || vectors < 1 || vectors > 64 || chunk * vectors > 65507) return 2;
    }
    unsigned long total = DEFAULT_TOTAL;
    if (argc == 5 || argc == 7) {
        const char *argument = argv[argc - 1];
        total = strtoul(argument, &end, 10);
        if (!*argument || *end || !total || total > MAX_TOTAL) return 2;
    }
    uint8_t *payload = malloc(total);
    if (!payload) { perror("malloc"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    for (size_t i = 0; i < total; ++i) payload[i] = (uint8_t)(i * 17u + 3u);
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
    while (sent < total) {
        size_t want = total - sent < chunk * vectors ? total - sent : chunk * vectors;
        ssize_t n;
        if (!vectored) n = send(fd, payload + sent, want, MSG_NOSIGNAL);
        else {
            struct iovec iov[64];
            size_t offset = 0, count = 0;
            while (offset < want) {
                size_t length = want - offset < chunk ? want - offset : chunk;
                iov[count++] = (struct iovec){ .iov_base = payload + sent + offset, .iov_len = length };
                offset += length;
            }
            if (vectored == 1) n = writev(fd, iov, (int)count);
            else {
                struct msghdr message = { .msg_iov = iov, .msg_iovlen = count };
                n = sendmsg(fd, &message, MSG_NOSIGNAL);
            }
        }
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
    free(payload);
    printf("TCP_CHUNKS_OK bytes=%zu chunk=%lu vectors=%lu mode=%s calls=%zu partials=%zu\n",
           sent, chunk, vectors, vectored == 1 ? "writev" : vectored == 2 ? "sendmsg" : "send", calls, partials);
    return 0;
}

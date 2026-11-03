#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    static const char expected[] = "Brook guarded recovery fixture\n";
    char bytes[sizeof(expected)] = {0};
    int fd = open("/nix/brook-recovery-known", O_RDONLY);
    if (fd < 0) { perror("recovery mount open"); return 1; }
    ssize_t n = read(fd, bytes, sizeof(bytes));
    if (close(fd) || n != (ssize_t)(sizeof(expected) - 1) ||
        memcmp(bytes, expected, sizeof(expected) - 1)) {
        fputs("RECOVERY_NORMAL_DATA_READ_FAILED\n", stderr);
        return 1;
    }
    puts("RECOVERY_NORMAL_DATA_READ_PASS");
    return 0;
}

/* Make one authenticated RAM transfer executable in a maintenance boot. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/update/payload.h"

int main(int argc, char **argv) {
    if (argc != 3 || geteuid() != 0 || strlen(argv[1]) != 32 || strlen(argv[2]) != 64)
        return 2;
    for (unsigned i = 0; i < 32; ++i)
        if (!((argv[1][i] >= '0' && argv[1][i] <= '9') ||
              (argv[1][i] >= 'a' && argv[1][i] <= 'f'))) return 2;
    char path[96];
    if (snprintf(path, sizeof(path), "/data/.brook-transfer/%s.bin", argv[1]) >= (int)sizeof(path)) return 2;
    uint8_t expected[32];
    for (unsigned i = 0; i < 32; ++i) {
        unsigned n = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = argv[2][2*i+j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 2;
            n = 16*n + (unsigned)(c <= '9' ? c-'0' : c-'a'+10);
        }
        expected[i] = (uint8_t)n;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        st.st_size <= 0 || st.st_size > 8*1024*1024) return 1;
    BrookSha256 sha; BrookShaInit(&sha);
    uint8_t buffer[4096], actual[32];
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return 1; }
        if (!n) break;
        BrookShaAdd(&sha, buffer, (size_t)n);
    }
    BrookShaFinish(&sha, actual);
    if (!BrookUpdateEqual(actual, expected, 32) || fchmod(fd, 0700) || close(fd)) return 1;
    puts("RAM_EXEC_STAGE_OK");
    return 0;
}

#include "hardware_store_validate.h"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
    assert(argc == 3);
    int fd = open(argv[1], O_RDONLY); assert(fd >= 0);
    assert(hardware_store_validate(fd, argv[2], strlen(argv[2])) == 0);
    char bad[97]; assert(strlen(argv[2]) < sizeof(bad)); strcpy(bad, argv[2]);
    bad[strlen(bad)-1] = bad[strlen(bad)-1] == 'a' ? 'b' : 'a';
    assert(hardware_store_validate(fd, bad, strlen(bad)) == -EINVAL);
    assert(hardware_store_validate(fd, "invalid", 7) == -EINVAL);
    assert(hardware_store_validate(-1, argv[2], strlen(argv[2])) == -EBADF);
    char name[] = "/tmp/brook-store-validator-XXXXXX";
    int tmp = mkstemp(name); assert(tmp >= 0); unlink(name);
    struct stat st{}; assert(fstat(fd, &st) == 0);
    assert(ftruncate(tmp, st.st_size) == 0); // sparse; only metadata bytes are written
    char header[8192]; assert(pread(fd, header, sizeof(header), 0) == sizeof(header));
    assert(pwrite(tmp, header, sizeof(header), 0) == sizeof(header));
    assert(hardware_store_validate(tmp, argv[2], strlen(argv[2])) == 0);
    char dirty[2] = {0, 0}; assert(pwrite(tmp, dirty, 2, 1082) == 2);
    assert(hardware_store_validate(tmp, argv[2], strlen(argv[2])) == -EINVAL);
    assert(ftruncate(tmp, 8192) == 0);
    assert(hardware_store_validate(tmp, argv[2], strlen(argv[2])) == -EINVAL);
    close(tmp);
    close(fd);
    puts("hardware store validator passed");
}

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct file { const char *path; int fd; struct stat st; };
static void fatal(const char *s) { perror(s); exit(2); }
static void open_file(struct file *file, const char *path)
{
    file->path = path;
    file->fd = open(path, O_RDONLY);
    if (file->fd < 0 || fstat(file->fd, &file->st)) fatal(path);
    if (!S_ISREG(file->st.st_mode) || file->st.st_size <= 0 || file->st.st_size > 64*1024*1024) {
        fprintf(stderr, "requires a regular file between 1 byte and 64 MiB: %s\n", path);
        exit(2);
    }
    printf("MMAPCACHECHECK FILE %s dev=%ju ino=%ju size=%jd\n", path,
           (uintmax_t)file->st.st_dev, (uintmax_t)file->st.st_ino, (intmax_t)file->st.st_size);
    fflush(stdout);
}
static void read_exact(int fd, unsigned char *data, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, data+done, size-done, offset+done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal("pread (files must stay unchanged)");
        done += n;
    }
}
static int compare_mapping(struct file *file, const char *label)
{
    size_t length = file->st.st_size;
    unsigned char *mapped = mmap(NULL, length, PROT_READ, MAP_PRIVATE, file->fd, 0);
    if (mapped == MAP_FAILED) fatal("mmap");
    unsigned char direct[4096];
    uint64_t read_hash = 14695981039346656037ULL, map_hash = read_hash;
    size_t first = length, differences = 0;
    unsigned int read_byte = 0, map_byte = 0;
    for (size_t offset = 0; offset < length; offset += sizeof(direct)) {
        size_t n = length-offset < sizeof(direct) ? length-offset : sizeof(direct);
        read_exact(file->fd, direct, n, offset);
        for (size_t i = 0; i < n; ++i) {
            unsigned char actual = mapped[offset+i];
            read_hash = (read_hash ^ direct[i]) * 1099511628211ULL;
            map_hash = (map_hash ^ actual) * 1099511628211ULL;
            if (direct[i] != actual) {
                if (!differences) { first = offset+i; read_byte = direct[i]; map_byte = actual; }
                ++differences;
            }
        }
    }
    printf("MMAPCACHECHECK %s %s path=%s pread_fnv=%016" PRIx64 " mmap_fnv=%016" PRIx64
           " differences=%zu", differences ? "FAIL" : "PASS", label, file->path,
           read_hash, map_hash, differences);
    if (differences) printf(" first_offset=0x%zx pread=%02x mmap=%02x", first, read_byte, map_byte);
    putchar('\n'); fflush(stdout);
    if (munmap(mapped, length)) fatal("munmap");
    return differences != 0;
}
static int different_contents(struct file *a, struct file *b)
{
    unsigned char left[4096], right[4096];
    size_t common = a->st.st_size < b->st.st_size ? a->st.st_size : b->st.st_size;
    for (size_t offset = 0; offset < common; offset += sizeof(left)) {
        size_t n = common-offset < sizeof(left) ? common-offset : sizeof(left);
        read_exact(a->fd, left, n, offset); read_exact(b->fd, right, n, offset);
        if (memcmp(left, right, n)) return 1;
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s --diagnose existing-file\n"
                        "   or: %s file-on-mount-A file-on-mount-B (equal inode, differing bytes)\n", argv[0], argv[0]);
        return 2;
    }
    struct file a, b;
    int failures = 0;
    if (!strcmp(argv[1], "--diagnose")) {
        open_file(&a, argv[2]);
        failures = compare_mapping(&a, "diagnostic pread versus private mmap");
        close(a.fd);
    } else {
        open_file(&a, argv[1]); open_file(&b, argv[2]);
        if (a.st.st_ino != b.st.st_ino || !different_contents(&a, &b)) {
            fprintf(stderr, "collision fixture requires equal inode numbers and differing bytes at overlapping offsets\n");
            return 2;
        }
        /* The paths must be on different mounts; Brook's st_dev is still 0. */
        failures += compare_mapping(&a, "prime mount A");
        failures += compare_mapping(&b, "same inode on mount B");
        failures += compare_mapping(&b, "repeat mount B");
        close(a.fd);
        struct stat prior = a.st;
        open_file(&a, argv[1]);
        if (a.st.st_ino != prior.st_ino || a.st.st_size != prior.st_size) fatal("fixture changed");
        failures += compare_mapping(&a, "reopened mount A after B");
        close(a.fd); close(b.fd);
    }
    printf("MMAPCACHECHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

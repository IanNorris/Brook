#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void fatal(const char *s) { perror(s); exit(1); }
static void check(int ok, const char *label)
{
    printf("FD3CHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static const char child_data[] = "fd3 exec file payload\n";
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--child")) {
        int fd = open(argv[2], O_CREAT|O_EXCL|O_RDWR, 0600);
        if (fd != 3) _exit(90);
        if (write(fd, child_data, sizeof(child_data)) != sizeof(child_data)) _exit(91);
        struct stat st;
        if (fstat(fd, &st) || st.st_size != sizeof(child_data)) _exit(92);
        close(fd);
        _exit(0);
    }
    int serial = argc == 2;
    if (argc < 2 || argc > 3 || argv[0][0] != '/' || argv[1][0] != '/' ||
        (argc == 3 && strcmp(argv[2], "--no-serial"))) {
        fprintf(stderr, "usage: /absolute/fd3check absolute-writable-directory [--no-serial]\n");
        return 2;
    }
    int saved_serial = -1;
    if (serial) {
        struct stat st;
        int flags = fcntl(3, F_GETFD);
        check(flags >= 0 && !(flags & FD_CLOEXEC) && fstat(3, &st) == 0 && S_ISCHR(st.st_mode),
              "inherited debug fd3 is a non-CLOEXEC character device");
        saved_serial = fcntl(3, F_DUPFD, 32);
        if (saved_serial < 0) fatal("save serial fd");
        static const char direct[] = "FD3CHECK SERIAL ORIGINAL\n";
        static const char duplicate[] = "FD3CHECK SERIAL DUPLICATE\n";
        check(write(3, direct, sizeof(direct)-1) == sizeof(direct)-1,
              "legacy serial write reports success");
        check(write(saved_serial, duplicate, sizeof(duplicate)-1) == sizeof(duplicate)-1,
              "duplicated serial descriptor reports success (verify UART sentinel)");
    }
    close(3);
    errno = 0;
    check(write(3, "X", 1) == -1 && errno == EBADF, "closed fd3 write returns EBADF");
    errno = 0;
    check(write(3, "", 0) == -1 && errno == EBADF, "closed fd3 zero-length write returns EBADF");
    char fixture[192], path[256], child_path[256];
    if (snprintf(fixture, sizeof(fixture), "%s/fd3check-XXXXXX", argv[1]) >= (int)sizeof(fixture)) return 2;
    if (!mkdtemp(fixture)) fatal("mkdtemp");
    snprintf(path, sizeof(path), "%s/file", fixture);
    snprintf(child_path, sizeof(child_path), "%s/child", fixture);
    int fd = open(path, O_CREAT|O_EXCL|O_RDWR, 0600);
    if (fd < 0) fatal("open file");
    check(fd == 3, "open reuses closed fd3");
    const unsigned char payload[] = {'A', 0, 'B', 'C', 'D', 'E'};
    unsigned char data[64] = {0};
    check(write(fd, payload, sizeof(payload)) == sizeof(payload), "binary write to reused fd3");
    check(pwrite(fd, "XY", 2, 2) == 2 && lseek(fd, 0, SEEK_CUR) == sizeof(payload),
          "pwrite at fd3 preserves sequential offset");
    struct iovec iov[2] = {{(void *)"12", 2}, {(void *)"34", 2}};
    check(writev(fd, iov, 2) == 4, "writev follows reused fd3 file type");
    const unsigned char expected[] = {'A', 0, 'X', 'Y', 'D', 'E', '1', '2', '3', '4'};
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == sizeof(expected) &&
          pread(fd, data, sizeof(data), 0) == sizeof(expected) &&
          !memcmp(data, expected, sizeof(expected)), "fd3 writes persisted exact bytes and size");
    int duplicate_file = dup(fd);
    if (duplicate_file < 0) fatal("dup file");
    close(fd);
    int placeholder = open("/dev/null", O_WRONLY);
    check(placeholder == 3 && dup2(duplicate_file, 3) == 3,
          "dup2 replaces fd3 device with file");
    check(pwrite(3, "Z", 1, 0) == 1 && pread(duplicate_file, data, 1, 0) == 1 && data[0] == 'Z',
          "dup2 file at fd3 uses shared inode");
    close(duplicate_file); close(3);

    int pair[2];
    if (pipe2(pair, O_NONBLOCK)) fatal("pipe2");
    int reader = fcntl(pair[0], F_DUPFD, 40);
    if (reader < 0 || dup2(pair[1], 3) != 3) fatal("pipe dup2");
    if (pair[0] != 3) close(pair[0]);
    if (pair[1] != 3) close(pair[1]);
    check(write(3, "P", 1) == 1 && read(reader, data, 1) == 1 && data[0] == 'P',
          "pipe write endpoint duplicated to fd3 delivers data");
    close(reader); close(3);
    int event = eventfd(0, EFD_NONBLOCK);
    uint64_t sent = 9, received = 0;
    check(event == 3 && write(event, &sent, sizeof(sent)) == sizeof(sent) &&
          read(event, &received, sizeof(received)) == sizeof(received) && received == sent,
          "eventfd reusing fd3 updates counter");
    close(event);

    if (serial) {
        if (dup2(saved_serial, 3) != 3) fatal("restore serial before exec test");
    } else if (open("/dev/null", O_WRONLY) != 3) fatal("occupy fd3");
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) fatal("fork");
    if (!child) {
        close(3);
        execl(argv[0], argv[0], "--child", child_path, (char *)NULL);
        _exit(93);
    }
    int status = 0;
    pid_t got;
    do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
    check(got == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child closes fd3 then exec opens and writes file at fd3");
    fd = open(child_path, O_RDONLY);
    check(fd >= 0 && read(fd, data, sizeof(data)) == sizeof(child_data) &&
          !memcmp(data, child_data, sizeof(child_data)), "exec child output persisted");
    if (fd >= 0) close(fd);
    if (saved_serial >= 0) close(saved_serial); else close(3);
    if (unlink(path) || unlink(child_path) || rmdir(fixture)) fatal("cleanup");
    printf("FD3CHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

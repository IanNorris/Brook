// Shared open file description acceptance (refactor issue H-20).
//
// Run from INIT.RC as `run /boot/OPENFILE.ELF`; scripts/test_open_file_qemu.py
// builds the disposable image. Prints OPEN_FILE_OK or OPEN_FILE_FAIL <why>.
//
// Linux semantics checked: dup/dup2/dup3/F_DUPFD, fork and SCM_RIGHTS share
// the file offset and status flags; FD_CLOEXEC stays per descriptor; two
// open() calls are independent; O_APPEND always writes at EOF. With
// /boot/BUSYBOX present, the same is checked through real shell redirection:
// `cmd >f 2>&1` and `{ a; b; } >f` must append rather than overwrite.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char* self;
static char dir[200];
static int failures;

static void fail(const char* format, ...)
{
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    printf("OPEN_FILE_FAIL %s\n", text);
    fflush(stdout);
    ++failures;
}

#define EXPECT(cond, ...) do { if (!(cond)) fail(__VA_ARGS__); } while (0)

static void path(char* out, size_t size, const char* name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

// Whole-file contents through an independent description.
static int slurp(const char* file, char* out, size_t size)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, size - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    return (int)n;
}

static void expect_file(const char* file, const char* expected, const char* label)
{
    char text[256];
    int n = slurp(file, text, sizeof(text));
    EXPECT(n >= 0 && strcmp(text, expected) == 0, "%s: file holds \"%s\" (n=%d), expected \"%s\"",
           label, n >= 0 ? text : "?", n, expected);
}

static int create(const char* file, int extra)
{
    return open(file, O_WRONLY | O_CREAT | O_TRUNC | extra, 0644);
}

static int wait_child(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static void test_dup_family(void)
{
    char file[320];
    path(file, sizeof(file), "dup.txt");
    int fd = create(file, 0);
    EXPECT(fd >= 0, "dup: open: %d", errno);
    if (fd < 0) return;
    int d = dup(fd);
    EXPECT(write(fd, "a", 1) == 1 && write(d, "b", 1) == 1, "dup: write");
    expect_file(file, "ab", "dup then write a,b");
    EXPECT(lseek(d, 0, SEEK_CUR) == 2 && lseek(fd, 0, SEEK_CUR) == 2, "dup: shared offset");
    int d2 = dup2(fd, 40), d3 = dup3(fd, 41, O_CLOEXEC), df = fcntl(fd, F_DUPFD, 50);
    EXPECT(d2 == 40 && d3 == 41 && df >= 50, "dup2/dup3/F_DUPFD numbers %d %d %d", d2, d3, df);
    EXPECT(write(d2, "c", 1) == 1 && write(d3, "d", 1) == 1 && write(df, "e", 1) == 1, "dup2/3/F_DUPFD write");
    expect_file(file, "abcde", "dup2/dup3/F_DUPFD continue the shared offset");
    EXPECT(lseek(fd, 1, SEEK_SET) == 1 && lseek(d3, 0, SEEK_CUR) == 1, "lseek through one alias moves all");
    // FD_CLOEXEC is per descriptor.
    EXPECT(fcntl(fd, F_GETFD) == 0 && fcntl(d3, F_GETFD) == FD_CLOEXEC, "dup3 O_CLOEXEC is per fd");
    EXPECT(fcntl(d2, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == 0 && fcntl(d, F_GETFD) == 0,
           "F_SETFD stays on its own descriptor");
    // Status flags are shared: O_APPEND through one alias applies to all.
    EXPECT(fcntl(df, F_SETFL, O_APPEND) == 0 && (fcntl(fd, F_GETFL) & O_APPEND), "F_SETFL O_APPEND shared");
    EXPECT(write(fd, "f", 1) == 1, "append write");
    expect_file(file, "abcdef", "O_APPEND set through an alias writes at EOF");
    EXPECT((fcntl(fd, F_GETFL) & O_ACCMODE) == O_WRONLY, "F_SETFL keeps access mode");
    close(fd); close(d); close(d2); close(d3); close(df);
}

static void test_independent_and_append(void)
{
    char file[320];
    path(file, sizeof(file), "independent.txt");
    int a = create(file, 0), b = open(file, O_WRONLY);
    EXPECT(a >= 0 && b >= 0, "independent: open");
    EXPECT(write(a, "12", 2) == 2 && write(b, "X", 1) == 1, "independent: write");
    expect_file(file, "X2", "two open() calls keep independent offsets");
    EXPECT(lseek(a, 0, SEEK_CUR) == 2 && lseek(b, 0, SEEK_CUR) == 1, "independent offsets");
    close(a); close(b);
    a = open(file, O_WRONLY | O_APPEND); b = open(file, O_WRONLY | O_APPEND);
    EXPECT(write(a, "p", 1) == 1 && write(b, "q", 1) == 1 && write(a, "r", 1) == 1, "append: write");
    expect_file(file, "X2pqr", "O_APPEND descriptions always write at EOF");
    close(a); close(b);
    // F_GETFL reports neither O_CREAT/O_TRUNC/O_EXCL nor O_CLOEXEC (M-50).
    int fd = open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int flags = fcntl(fd, F_GETFL) & ~0100000; // ignore O_LARGEFILE
    EXPECT(flags == O_RDWR, "F_GETFL after O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC is 0x%x", flags);
    EXPECT(fcntl(fd, F_SETFL, O_NONBLOCK | O_TRUNC) == 0 &&
           (fcntl(fd, F_GETFL) & ~0100000) == (O_RDWR | O_NONBLOCK), "F_SETFL ignores O_TRUNC");
    close(fd);
}

static void test_fork(void)
{
    char file[320];
    path(file, sizeof(file), "fork.txt");
    int fd = create(file, 0);
    EXPECT(write(fd, "p", 1) == 1, "fork: parent write");
    pid_t pid = fork();
    if (pid == 0) {
        _exit(write(fd, "c", 1) == 1 && lseek(fd, 0, SEEK_CUR) == 2 ? 0 : 1);
    }
    EXPECT(wait_child(pid) == 0, "fork: child continued at offset 1");
    EXPECT(lseek(fd, 0, SEEK_CUR) == 2, "fork: child write moved parent offset to %ld",
           (long)lseek(fd, 0, SEEK_CUR));
    EXPECT(write(fd, "q", 1) == 1, "fork: parent write after child");
    expect_file(file, "pcq", "fork child continues the parent's offset");
    close(fd);

    // A forked child's F_SETFL O_NONBLOCK reaches the parent (pipe and file).
    int pipes[2];
    EXPECT(pipe(pipes) == 0, "pipe");
    pid = fork();
    if (pid == 0) _exit(fcntl(pipes[0], F_SETFL, O_NONBLOCK) == 0 ? 0 : 1);
    EXPECT(wait_child(pid) == 0, "child F_SETFL");
    EXPECT(fcntl(pipes[0], F_GETFL) & O_NONBLOCK, "child's F_SETFL O_NONBLOCK reaches the parent");
    char byte;
    EXPECT(read(pipes[0], &byte, 1) == -1 && errno == EAGAIN, "parent read is now nonblocking (errno %d)", errno);
    EXPECT(!(fcntl(pipes[1], F_GETFL) & O_NONBLOCK), "the other pipe end is a separate description");
    EXPECT(lseek(pipes[0], 0, SEEK_CUR) == -1 && errno == ESPIPE, "lseek on a pipe is ESPIPE");
    close(pipes[0]); close(pipes[1]);
}

static int recv_fd(int socket, int flags)
{
    char data;
    struct iovec io = {&data, 1};
    union { struct cmsghdr header; char space[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr message = {0};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control.space; message.msg_controllen = sizeof(control.space);
    if (recvmsg(socket, &message, flags) != 1) return -1;
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    return fd;
}

static int send_fd(int socket, int fd)
{
    char data = 'x';
    struct iovec io = {&data, 1};
    union { struct cmsghdr header; char space[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {0};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control.space; message.msg_controllen = sizeof(control.space);
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}

static void test_scm_rights(void)
{
    char file[320];
    path(file, sizeof(file), "rights.txt");
    int fd = open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int pair[2];
    EXPECT(fd >= 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "rights: setup");
    EXPECT(lseek(pair[0], 0, SEEK_CUR) == -1 && errno == ESPIPE, "lseek on a socket is ESPIPE (L-63)");
    EXPECT(write(fd, "s", 1) == 1 && send_fd(pair[0], fd) == 0, "rights: send");
    int received = recv_fd(pair[1], 0);
    EXPECT(received >= 0 && received != fd, "rights: receive %d", received);
    if (received >= 0) {
        EXPECT(fcntl(received, F_GETFD) == 0, "received fd has no FD_CLOEXEC without MSG_CMSG_CLOEXEC");
        EXPECT(lseek(received, 0, SEEK_CUR) == 1, "received description has the sender's offset");
        EXPECT(write(received, "r", 1) == 1 && lseek(fd, 0, SEEK_CUR) == 2, "receiver write moves sender offset");
        EXPECT(fcntl(received, F_SETFL, O_NONBLOCK) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK),
               "receiver F_SETFL reaches sender");
        close(received);
    }
    EXPECT(send_fd(pair[0], fd) == 0, "rights: second send");
    received = recv_fd(pair[1], MSG_CMSG_CLOEXEC);
    EXPECT(received >= 0 && fcntl(received, F_GETFD) == FD_CLOEXEC, "MSG_CMSG_CLOEXEC sets FD_CLOEXEC");
    close(received);
    expect_file(file, "sr", "SCM_RIGHTS shares the description");
    close(fd); close(pair[0]); close(pair[1]);
}

// exec: FD_CLOEXEC closes only its own descriptor; the shared description
// survives through the other alias with its offset.
static void test_exec(void)
{
    char file[320];
    path(file, sizeof(file), "exec.txt");
    int fd = create(file, 0);
    EXPECT(write(fd, "e", 1) == 1, "exec: write");
    EXPECT(dup2(fd, 60) == 60 && dup3(fd, 61, O_CLOEXEC) == 61, "exec: dup");
    pid_t pid = fork();
    if (pid == 0) {
        char* argv[] = {(char*)self, "exec-child", NULL};
        execv(self, argv);
        _exit(3);
    }
    EXPECT(wait_child(pid) == 0, "exec: CLOEXEC alias closed, plain alias kept with offset");
    expect_file(file, "ex", "exec child writes at the shared offset");
    close(fd); close(60); close(61);
}

static int exec_child(void)
{
    if (fcntl(61, F_GETFD) != -1 || errno != EBADF) return 1;
    if (lseek(60, 0, SEEK_CUR) != 1) return 2;
    return write(60, "x", 1) == 1 ? 0 : 4;
}

// Real shell redirections. Each external command is a separate process that
// inherits the descriptors, so before H-20 the later writer overwrote the
// earlier output from offset 0.
static void test_shell(void)
{
    const char* shell = getenv("OPEN_FILE_SHELL") ? getenv("OPEN_FILE_SHELL") : "/boot/BUSYBOX";
    if (access(shell, X_OK) != 0) {
        printf("OPEN_FILE_SHELL_SKIPPED no %s\n", shell);
        return;
    }
    char script[2048], file[320], group[320], builtin[320];
    path(file, sizeof(file), "shell-2to1.txt");
    path(group, sizeof(group), "shell-group.txt");
    path(builtin, sizeof(builtin), "shell-builtin.txt");
    snprintf(script, sizeof(script),
             "%s emit A >%s 2>&1; { %s emit B; %s emit C; } >%s 2>/dev/null; "
             "{ echo one; echo two >&2; } >%s 2>&1",
             self, file, self, self, group, builtin);
    pid_t pid = fork();
    if (pid == 0) {
        char* argv[] = {"sh", "-c", script, NULL};
        execv(shell, argv);
        _exit(127);
    }
    EXPECT(wait_child(pid) == 0, "shell script exit");
    expect_file(file, "A-out\nA-err\n", "cmd >f 2>&1 interleaves stdout and stderr");
    expect_file(group, "B-out\nC-out\n", "{ a; b; } >f appends both commands");
    expect_file(builtin, "one\ntwo\n", "builtin { echo; echo >&2; } >f 2>&1");
    printf("OPEN_FILE_SHELL_DONE\n");
}

// `emit TAG`: one line to stdout then one to stderr (both may be the file).
static int emit(const char* tag)
{
    char line[64];
    int n = snprintf(line, sizeof(line), "%s-out\n", tag);
    if (write(1, line, n) != n) return 1;
    n = snprintf(line, sizeof(line), "%s-err\n", tag);
    return write(2, line, n) == n ? 0 : 1;
}

int main(int argc, char** argv)
{
    self = argv[0];
    if (argc > 2 && strcmp(argv[1], "emit") == 0) return emit(argv[2]);
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0) return exec_child();
    // OPEN_FILE_DIR lets the same binary validate its expectations on Linux.
    if (getenv("OPEN_FILE_DIR")) snprintf(dir, sizeof(dir), "%s", getenv("OPEN_FILE_DIR"));
    static const char* candidates[] = {"/tmp", "/data", "/boot", "/"};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && !dir[0]; ++i) {
        char probe[320];
        snprintf(probe, sizeof(probe), "%s/openfile-probe", candidates[i]);
        int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) continue;
        close(fd); unlink(probe);
        snprintf(dir, sizeof(dir), "%s", strcmp(candidates[i], "/") ? candidates[i] : "");
        if (!dir[0]) snprintf(dir, sizeof(dir), "/.");
    }
    if (!dir[0]) { printf("OPEN_FILE_FAIL no writable directory\n"); return 1; }
    printf("OPEN_FILE_START dir=%s self=%s\n", dir, self);
    fflush(stdout);
    test_dup_family();
    test_independent_and_append();
    test_fork();
    test_scm_rights();
    test_exec();
    test_shell();
    if (failures) { printf("OPEN_FILE_FAIL count=%d\n", failures); fflush(stdout); return 1; }
    printf("OPEN_FILE_OK\n");
    fflush(stdout);
    return 0;
}

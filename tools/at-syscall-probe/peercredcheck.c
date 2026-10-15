#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do { if (!(x)) { printf("PEERCREDCHECK FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); ++failures; } } while (0)
static void expect(int fd, pid_t pid, uid_t uid, gid_t gid) {
    struct ucred expected = {pid, uid, gid};
    for (socklen_t n = 0; n <= 16; ++n) {
        unsigned char out[16]; memset(out, 0xa5, sizeof(out));
        socklen_t size = n, copied = n < sizeof(expected) ? n : sizeof(expected);
        CHECK(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, out, &size) == 0);
        CHECK(size == copied && !memcmp(out, &expected, copied));
        for (unsigned i = copied; i < sizeof(out); ++i) CHECK(out[i] == 0xa5);
    }
    socklen_t zero = 0;
    CHECK(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, NULL, &zero) == 0 && !zero);
}
static void *thread_pair(void *unused) {
    (void)unused; int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    expect(pair[0], getpid(), geteuid(), getegid());
    close(pair[0]); close(pair[1]); return NULL;
}
int main(int argc, char **argv) {
    syscall(SYS_alarm, 30);
    int require_root = argc == 2 && !strcmp(argv[1], "--require-root");
    CHECK(!require_root || geteuid() == 0);
    pid_t original = getpid(); uid_t uid = geteuid(); gid_t gid = getegid();
    int unbound = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(unbound >= 0);
    expect(unbound, 0, (uid_t)-1, (gid_t)-1); close(unbound);
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    expect(pair[0], original, uid, gid);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        if (!uid) { CHECK(setgid(1000) == 0); CHECK(setuid(1000) == 0); }
        expect(pair[1], original, uid, gid); // inherited peer identity stays creator's
        close(pair[0]); close(pair[1]); _exit(failures ? 1 : 0);
    }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(pair[0]); expect(pair[1], original, uid, gid); close(pair[1]);
    pthread_t thread; CHECK(pthread_create(&thread, NULL, thread_pair, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    char path[100]; snprintf(path, sizeof(path), "/tmp/peercred-%ld.sock", (long)original);
    struct sockaddr_un addr = {.sun_family = AF_UNIX}; strcpy(addr.sun_path, path);
    unlink(path); int listener = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(listener >= 0);
    mode_t old = umask(0);
    CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0); umask(old);
    CHECK(listen(listener, 4) == 0);
    expect(listener, original, uid, gid);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(listener);
        if (!uid) { CHECK(setgid(1000) == 0); CHECK(setuid(1000) == 0); }
        int client = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(client >= 0);
        CHECK(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        expect(client, original, uid, gid);
        CHECK(write(client, "x", 1) == 1); char ack;
        CHECK(read(client, &ack, 1) == 1); close(client); _exit(failures ? 1 : 0);
    }
    int accepted = accept(listener, NULL, NULL); CHECK(accepted >= 0);
    expect(accepted, child, uid ? uid : 1000, uid ? gid : 1000);
    char marker; CHECK(read(accepted, &marker, 1) == 1 && marker == 'x');
    CHECK(write(accepted, "y", 1) == 1); close(accepted);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    // An inherited listener retains the process identity from listen(), not accept().
    child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(listen(listener, 4) == 0); // repeated listen must preserve initial identity
        int incoming = accept(listener, NULL, NULL); CHECK(incoming >= 0);
        expect(incoming, original, uid, gid);
        CHECK(write(incoming, "z", 1) == 1); close(incoming); close(listener); _exit(failures ? 1 : 0);
    }
    int client = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(client >= 0);
    CHECK(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    expect(client, original, uid, gid);
    CHECK(read(client, &marker, 1) == 1 && marker == 'z'); close(client);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(listener); unlink(path);
    printf("PEERCREDCHECK %s failures=%d root_case=%d\n", failures ? "FAIL" : "PASS", failures, !uid);
    return failures ? 1 : 0;
}

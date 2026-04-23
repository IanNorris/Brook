/*
 * scm_rights_test.c — Brook OS SCM_RIGHTS fd-passing smoke test.
 *
 * Exercises the kernel path landed in commit 8161add:
 *   parent  creates an AF_UNIX listening socket and fork()s a child;
 *   child   connects, ftruncates a memfd, writes a sentinel to it,
 *           then sends the memfd fd to the parent via SCM_RIGHTS;
 *   parent  accepts, recvmsg()s the cmsg, mmaps the received fd, and
 *           verifies the sentinel bytes match.
 *
 * This is the concrete plumbing libwayland uses for wl_shm. Passing
 * this test is the kernel-side precondition for libwayland to work.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1u
#endif
#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif

#define SOCKET_PATH "/tmp/.scm-rights-test"
#define SHM_SIZE    4096
#define SENTINEL    0xCAFEBABEu

static int memfd_create_brook(const char* name, unsigned flags) {
    return (int)syscall(SYS_memfd_create, name, flags);
}

static int send_fd(int sock, int fd) {
    char payload = 'F';
    struct iovec iov = { .iov_base = &payload, .iov_len = 1 };
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof(ctl));

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof(ctl);

    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));

    ssize_t n = sendmsg(sock, &msg, 0);
    if (n < 0) { perror("sendmsg"); return -1; }
    return 0;
}

static int recv_fd(int sock) {
    char payload = 0;
    struct iovec iov = { .iov_base = &payload, .iov_len = 1 };
    char ctl[CMSG_SPACE(sizeof(int))];

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof(ctl);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0) { perror("recvmsg"); return -1; }
    if (n == 0) { fprintf(stderr, "recvmsg: EOF before cmsg\n"); return -1; }

    for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int fd;
            memcpy(&fd, CMSG_DATA(cm), sizeof(int));
            return fd;
        }
    }
    fprintf(stderr, "recvmsg: no SCM_RIGHTS cmsg (payload byte='%c' ctllen=%zu)\n",
            payload, (size_t)msg.msg_controllen);
    return -1;
}

static int run_client(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("client: socket"); return 1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    for (int i = 0; i < 50; i++) {
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) break;
        usleep(20 * 1000);
    }

    int fd = memfd_create_brook("scm-test-shm", MFD_CLOEXEC);
    if (fd < 0) { perror("client: memfd_create"); return 1; }
    if (ftruncate(fd, SHM_SIZE) < 0) { perror("client: ftruncate"); return 1; }

    uint32_t* p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("client: mmap"); return 1; }
    p[0] = SENTINEL;
    munmap(p, SHM_SIZE);

    printf("[client] wrote sentinel 0x%08X to memfd %d, sending via SCM_RIGHTS...\n",
            SENTINEL, fd);
    if (send_fd(sock, fd) < 0) { fprintf(stderr, "[client] send_fd failed\n"); return 1; }

    close(fd);
    close(sock);
    printf("[client] done\n");
    return 0;
}

static int run_server(void) {
    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("srv: socket"); return 1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 1) < 0) { perror("listen"); return 1; }

    int cli = accept(srv, NULL, NULL);
    if (cli < 0) { perror("accept"); return 1; }

    int rfd = recv_fd(cli);
    if (rfd < 0) { fprintf(stderr, "[server] recv_fd failed\n"); return 1; }
    printf("[server] got fd=%d via SCM_RIGHTS\n", rfd);

    uint32_t* p = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, rfd, 0);
    if (p == MAP_FAILED) { perror("server: mmap received fd"); return 1; }

    uint32_t got = p[0];
    munmap(p, SHM_SIZE);
    close(rfd);
    close(cli);
    close(srv);
    unlink(SOCKET_PATH);

    if (got != SENTINEL) {
        printf("[server] FAIL: expected 0x%08X, got 0x%08X\n", SENTINEL, got);
        return 1;
    }
    printf("[server] PASS: shared memory via SCM_RIGHTS verified (0x%08X)\n", got);
    return 0;
}

int main(void) {
    printf("=== SCM_RIGHTS FD-PASSING TEST ===\n");
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) { _exit(run_client()); }

    int rc = run_server();
    int st = 0;
    waitpid(pid, &st, 0);
    int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (rc == 0 && crc == 0) {
        printf("=== PASS ===\n");
        return 0;
    }
    printf("=== FAIL server=%d client=%d ===\n", rc, crc);
    return 1;
}

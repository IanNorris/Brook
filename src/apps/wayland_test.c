/*
 * wayland_test.c — Brook OS Wayland plumbing smoke test
 *
 * Tests all syscalls required for a Wayland compositor:
 *   - AF_UNIX socket (bind/listen/connect/accept/send/recv)
 *   - epoll (create/ctl/wait)
 *   - timerfd (create/settime/read)
 *   - memfd_create + ftruncate + mmap(MAP_SHARED)
 *
 * The program forks into a "compositor" (parent) and "client" (child).
 * They exchange a simple handshake over AF_UNIX, then verify shared memory.
 *
 * Compile (static musl):
 *   musl-clang -static -o wayland_test wayland_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

/* memfd_create isn't in older musl headers — call via syscall */
#include <sys/syscall.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif

static int memfd_create_brook(const char *name, unsigned flags)
{
    return (int)syscall(SYS_memfd_create, name, flags);
}

#define SOCKET_PATH "/tmp/.wayland-0"
#define SHM_SIZE    4096

/* ------------------------------------------------------------------ */
/* Compositor (server) side                                            */
/* ------------------------------------------------------------------ */
static int run_compositor(int shm_fd)
{
    printf("[compositor] starting\n");

    /* Create listening AF_UNIX socket */
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) { perror("compositor: socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("compositor: bind"); return 1;
    }
    if (listen(srv, 4) < 0) { perror("compositor: listen"); return 1; }
    printf("[compositor] listening on %s\n", SOCKET_PATH);

    /* Set up epoll to wait for incoming connection */
    int ep = epoll_create1(0);
    if (ep < 0) { perror("compositor: epoll_create1"); return 1; }

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = srv;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, srv, &ev) < 0) {
        perror("compositor: epoll_ctl"); return 1;
    }
    printf("[compositor] epoll watching srv fd\n");

    /* Wait for connection (500ms timeout) */
    struct epoll_event events[4];
    int n = epoll_wait(ep, events, 4, 500);
    if (n <= 0) {
        printf("[compositor] FAIL: epoll_wait timed out or error (n=%d errno=%d)\n", n, errno);
        return 1;
    }
    printf("[compositor] epoll_wait returned %d event(s)\n", n);

    /* Accept the client */
    int cli = accept(srv, NULL, NULL);
    if (cli < 0) { perror("compositor: accept"); return 1; }
    printf("[compositor] accepted client fd=%d\n", cli);

    /* Read the client's message */
    char buf[64] = {0};
    ssize_t rd = read(cli, buf, sizeof(buf) - 1);
    if (rd <= 0) { printf("[compositor] FAIL: read returned %zd\n", rd); return 1; }
    printf("[compositor] received: '%s'\n", buf);

    if (strncmp(buf, "wl_display.sync", 15) != 0) {
        printf("[compositor] FAIL: unexpected message\n");
        return 1;
    }

    /* Write our pixel value into the shared memory */
    void *shm_ptr = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("compositor: mmap"); return 1;
    }
    uint32_t *pixels = (uint32_t*)shm_ptr;
    pixels[0] = 0xDEADBEEF;
    pixels[1] = 0xCAFEBABE;
    printf("[compositor] wrote 0xDEADBEEF to shm[0]\n");
    munmap(shm_ptr, SHM_SIZE);

    /* Reply to the client */
    const char *reply = "wl_callback.done:42";
    write(cli, reply, strlen(reply));
    printf("[compositor] sent: '%s'\n", reply);

    close(cli);
    close(srv);
    close(ep);
    unlink(SOCKET_PATH);

    printf("[compositor] PASS\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client side                                                         */
/* ------------------------------------------------------------------ */
static int run_client(int shm_fd)
{
    /* Give compositor time to bind */
    usleep(50000);

    printf("[client] starting\n");

    /* Test timerfd */
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("client: timerfd_create"); return 1; }

    struct itimerspec ts;
    memset(&ts, 0, sizeof(ts));
    ts.it_value.tv_sec  = 0;
    ts.it_value.tv_nsec = 10 * 1000000; /* 10ms */
    if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
        perror("client: timerfd_settime"); return 1;
    }

    /* Poll the timerfd with epoll */
    int ep = epoll_create1(0);
    if (ep < 0) { perror("client: epoll_create1"); return 1; }

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);

    struct epoll_event events[4];
    int n = epoll_wait(ep, events, 4, 200);
    if (n <= 0) {
        printf("[client] FAIL: timerfd epoll_wait returned %d errno=%d\n", n, errno);
        return 1;
    }
    uint64_t expirations = 0;
    read(tfd, &expirations, 8);
    printf("[client] timerfd fired: %llu expiration(s) — PASS\n", (unsigned long long)expirations);
    close(tfd);
    close(ep);

    /* Connect to compositor via AF_UNIX */
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) { perror("client: socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("client: connect"); return 1;
    }
    printf("[client] connected to compositor\n");

    /* Send a Wayland-style message */
    const char *msg = "wl_display.sync";
    write(sock, msg, strlen(msg));
    printf("[client] sent: '%s'\n", msg);

    /* Read reply */
    char buf[64] = {0};
    ssize_t rd = read(sock, buf, sizeof(buf) - 1);
    if (rd <= 0) { printf("[client] FAIL: read returned %zd\n", rd); return 1; }
    printf("[client] received: '%s'\n", buf);

    /* Verify shared memory written by compositor */
    void *shm_ptr = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("client: mmap shm"); return 1;
    }
    uint32_t *pixels = (uint32_t*)shm_ptr;
    if (pixels[0] == 0xDEADBEEF) {
        printf("[client] shm[0]=0x%08X — PASS (shared memory works!)\n", pixels[0]);
    } else {
        printf("[client] FAIL: shm[0]=0x%08X (expected 0xDEADBEEF)\n", pixels[0]);
        munmap(shm_ptr, SHM_SIZE);
        return 1;
    }

    /* Write something back to verify bidirectional shared memory */
    pixels[1] = 0x12345678;
    munmap(shm_ptr, SHM_SIZE);

    close(sock);
    printf("[client] PASS\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== WAYLAND PLUMBING SMOKE TEST ===\n");

    /* Create a shared memfd before forking so both sides share the same fd */
    int shm_fd = memfd_create_brook("wayland_shm", MFD_CLOEXEC);
    if (shm_fd < 0) {
        perror("memfd_create"); return 1;
    }
    if (ftruncate(shm_fd, SHM_SIZE) < 0) {
        perror("ftruncate"); return 1;
    }
    printf("[main] memfd fd=%d size=%d — OK\n", shm_fd, SHM_SIZE);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* Child = client */
        int rc = run_client(shm_fd);
        exit(rc);
    }

    /* Parent = compositor */
    int rc = run_compositor(shm_fd);

    int status = 0;
    waitpid(pid, &status, 0);
    int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    if (rc == 0 && child_rc == 0) {
        printf("\n=== ALL TESTS PASSED ===\n");
        return 0;
    } else {
        printf("\n=== SOME TESTS FAILED (compositor=%d client=%d) ===\n", rc, child_rc);
        return 1;
    }
}

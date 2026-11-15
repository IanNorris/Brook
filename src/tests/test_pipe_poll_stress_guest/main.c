// Pipe poll lifetime stress at a full descriptor table.
//
// Reproduces the Steam gen 415 workload shape: a process whose fd table is
// (nearly) full keeps creating pipes and socketpairs (most fail EMFILE),
// passes pipe and AF_UNIX descriptors over SCM_RIGHTS (receives hit EMFILE
// and MSG_CTRUNC), dups and closes them, forks children that inherit the full
// table and exit, while other threads ppoll the same descriptors. Any pipe
// freed while still referenced shows up as a kernel fault in the poll
// subscription walk. Prints PIPE_POLL_STRESS_OK after the run.
//
// Usage: PIPESTRS.ELF [seconds]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define POOL 96
static atomic_int pool[POOL];      // churned descriptors (-1 = empty)
static atomic_int stop;
static atomic_long counters[8];
enum { C_POLL, C_CREATE, C_EMFILE, C_SEND, C_RECV, C_TRUNC, C_FORK, C_CLOSE };
static int rights[2];              // SCM_RIGHTS carrier (stream)
static int records[2];             // SCM_RIGHTS carrier (SOCK_SEQPACKET, as CEF uses)

static unsigned next_random(unsigned* state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}

static void put(int fd)
{
    if (fd < 0) return;
    // Never block a churn thread; this also flips the shared status of
    // received and dup'd descriptions while other threads poll them.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    for (int i = 0; i < POOL; ++i) {
        int expected = -1;
        if (atomic_compare_exchange_strong(&pool[i], &expected, fd)) return;
    }
    close(fd);
}

static int take(unsigned* seed)
{
    for (int tries = 0; tries < POOL; ++tries) {
        int i = next_random(seed) % POOL;
        int fd = atomic_exchange(&pool[i], -1);
        if (fd >= 0) return fd;
    }
    return -1;
}

static int peek(unsigned* seed)
{
    return atomic_load(&pool[next_random(seed) % POOL]);
}

static void send_fd(int socket, int fd)
{
    char byte = 'r';
    struct iovec io = {&byte, (socket == records[0] && (fd & 1)) ? 0u : 1u};
    union { struct cmsghdr h; char space[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {0};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control.space; message.msg_controllen = sizeof(control.space);
    struct cmsghdr* h = CMSG_FIRSTHDR(&message);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof(fd));
    if (sendmsg(socket, &message, MSG_DONTWAIT) >= 0) atomic_fetch_add(&counters[C_SEND], 1);
}

static void recv_fds(int socket, int small)
{
    char byte;
    struct iovec io = {&byte, 1};
    union { struct cmsghdr h; char space[CMSG_SPACE(4 * sizeof(int))]; } control;
    struct msghdr message = {0};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control.space;
    message.msg_controllen = small ? sizeof(struct cmsghdr) : sizeof(control.space);
    if (recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC) < 0) return;
    atomic_fetch_add(&counters[C_RECV], 1);
    if (message.msg_flags & MSG_CTRUNC) atomic_fetch_add(&counters[C_TRUNC], 1);
    for (struct cmsghdr* h = CMSG_FIRSTHDR(&message); h; h = CMSG_NXTHDR(&message, h)) {
        if (h->cmsg_type != SCM_RIGHTS) continue;
        int n = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (int i = 0; i < n; ++i) {
            int fd;
            memcpy(&fd, CMSG_DATA(h) + i * sizeof(int), sizeof(fd));
            put(fd);
        }
    }
}

// Creates pipes/socketpairs/dups near EMFILE and passes them around.
static void* churn(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    while (!atomic_load(&stop)) {
        int pair[2], r = next_random(&seed) % 8;
        if (r == 0 && pipe2(pair, O_CLOEXEC | O_NONBLOCK) == 0) {
            atomic_fetch_add(&counters[C_CREATE], 1); put(pair[0]); put(pair[1]);
        } else if (r == 1 && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) == 0) {
            atomic_fetch_add(&counters[C_CREATE], 1); put(pair[0]); put(pair[1]);
        } else if (r == 2) {
            int fd = peek(&seed);
            int copy = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;
            if (copy >= 0) put(copy); else if (errno == EMFILE) atomic_fetch_add(&counters[C_EMFILE], 1);
        } else if (r == 3) {
            int fd = peek(&seed);
            if (fd >= 0) send_fd((seed & 8) ? records[0] : rights[0], fd);
        } else if (r == 4) {
            recv_fds((seed & 8) ? records[1] : rights[1], seed & 2);
        } else if (r == 5) {
            int fd = take(&seed);
            if (fd >= 0) { close(fd); atomic_fetch_add(&counters[C_CLOSE], 1); }
        } else if (r == 6) {
            int fd = peek(&seed);
            if (fd >= 0) (void)write(fd, "x", 1); // pipes: wake pollers; read ends: EBADF
        } else {
            int fd = peek(&seed);
            char buffer[64];
            if (fd >= 0) (void)read(fd, buffer, sizeof(buffer)); // pool fds are nonblocking
        }
        if (errno == EMFILE) atomic_fetch_add(&counters[C_EMFILE], 1);
    }
    return NULL;
}

static void* poller(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 40503u + 7;
    struct timespec timeout = {0, 1000000};
    while (!atomic_load(&stop)) {
        struct pollfd fds[16];
        for (int i = 0; i < 16; ++i) {
            fds[i].fd = peek(&seed);
            fds[i].events = POLLIN | ((seed & 4) ? POLLOUT : 0);
        }
        (void)ppoll(fds, 16, &timeout, NULL);
        atomic_fetch_add(&counters[C_POLL], 1);
    }
    return NULL;
}

// CEF-style shared memory: memfd_create near EMFILE. Most calls fail with
// EMFILE; the gen 415 kernel freed the memfd object twice on that path.
static void* memfds(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2246822519u + 5;
    while (!atomic_load(&stop)) {
        int fd = memfd_create("pps-shm", MFD_CLOEXEC);
        if (fd >= 0) {
            atomic_fetch_add(&counters[C_CREATE], 1);
            if (next_random(&seed) & 1) put(fd); else close(fd);
        } else if (errno == EMFILE) atomic_fetch_add(&counters[C_EMFILE], 1);
    }
    return NULL;
}

// Named FIFOs (Steam's steam.pipe), AF_UNIX listen/connect/accept and epoll,
// all near EMFILE and feeding the shared pool.
static char fifo_path[64], socket_path[64];
static void* namespaces(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 977u + 3;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (listener >= 0 && (bind(listener, (struct sockaddr*)&address, sizeof(address)) || listen(listener, 8))) {
        close(listener); listener = -1;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    while (!atomic_load(&stop)) {
        int r = next_random(&seed) % 6, fd = -1;
        if (r == 0) fd = open(fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        else if (r == 1) fd = open(fifo_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        else if (r == 2) fd = open(fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        else if (r == 3) {
            fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd >= 0 && connect(fd, (struct sockaddr*)&address, sizeof(address))) { close(fd); fd = -1; }
        } else if (r == 4 && listener >= 0) fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        else if (ep >= 0) {
            int target = peek(&seed);
            struct epoll_event event = {EPOLLIN | EPOLLOUT, {.fd = target}};
            if (target >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, target, &event)) epoll_ctl(ep, EPOLL_CTL_DEL, target, NULL);
            struct epoll_event out[8];
            (void)epoll_wait(ep, out, 8, 1);
        }
        if (fd >= 0) { atomic_fetch_add(&counters[C_CREATE], 1); put(fd); }
        else if (errno == EMFILE) atomic_fetch_add(&counters[C_EMFILE], 1);
    }
    if (listener >= 0) close(listener);
    if (ep >= 0) close(ep);
    return NULL;
}

// In a forked child: ppoll inherited descriptors until the group exits.
static void* child_poller(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    for (;;) {
        struct pollfd fds[8];
        for (int i = 0; i < 8; ++i) { fds[i].fd = 3 + (int)(next_random(&seed) % 1000); fds[i].events = POLLIN | POLLOUT; }
        struct timespec timeout = {0, 2000000};
        (void)ppoll(fds, 8, &timeout, NULL);
    }
    return NULL;
}

// Forks children that inherit the full table, try to allocate, and exit.
static void* forker(void* arg)
{
    (void)arg;
    while (!atomic_load(&stop)) {
        pid_t pid = fork();
        if (pid == 0) {
            int pair[2];
            if (pipe(pair) == 0) { close(pair[0]); close(pair[1]); }
            // Pass inherited descriptors back to the parent before exiting.
            send_fd(records[0], 3 + (int)(getpid() % 64));
            send_fd(rights[0], 4 + (int)(getpid() % 64));
            pthread_t helper;
            int threaded = !(getpid() & 1) &&
                pthread_create(&helper, NULL, child_poller, (void*)(uintptr_t)getpid()) == 0;
            struct pollfd fd = {3, POLLIN, 0};
            (void)poll(&fd, 1, threaded ? 3 : 0);
            if (threaded && (getpid() & 2)) _exit(1); // exit_group while the helper is in ppoll
            char* argv[] = {"/nonexistent/pulseaudio", NULL};
            execv(argv[0], argv);
            _exit(1);
        }
        if (pid > 0) { waitpid(pid, NULL, 0); atomic_fetch_add(&counters[C_FORK], 1); }
        struct timespec pause = {0, 5000000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}

int main(int argc, char** argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < POOL; ++i) atomic_init(&pool[i], -1);
    const char* dir = access("/tmp", W_OK) == 0 ? "/tmp" : ".";
    snprintf(fifo_path, sizeof(fifo_path), "%s/pps-fifo-%d", dir, (int)getpid());
    snprintf(socket_path, sizeof(socket_path), "%s/pps-sock-%d", dir, (int)getpid());
    unlink(fifo_path); unlink(socket_path);
    if (mkfifo(fifo_path, 0600)) printf("PIPE_POLL_STRESS_NOTE mkfifo errno=%d\n", errno);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, rights)) { printf("PIPE_POLL_STRESS_FAIL socketpair\n"); return 1; }
    fcntl(rights[0], F_SETFL, O_NONBLOCK); fcntl(rights[1], F_SETFL, O_NONBLOCK);
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, records)) { printf("PIPE_POLL_STRESS_FAIL seqpacket\n"); return 1; }
    // Fill the table, keeping a handful of slots that churn frees and refills.
    int filler = 0, pair[2];
    while (pipe(pair) == 0) { filler += 2; }
    int extra = open("/dev/null", O_RDONLY);
    if (extra >= 0) ++filler;
    // Free a few low-numbered filler fds so EMFILE alternates with success.
    for (int fd = 40; fd < 48; ++fd) close(fd);
    printf("PIPE_POLL_STRESS_START filled=%d seconds=%d\n", filler, seconds);
    fflush(stdout);
    pthread_t threads[12];
    int n = 0;
    for (int i = 0; i < 3; ++i) pthread_create(&threads[n++], NULL, churn, (void*)(uintptr_t)(i + 1));
    for (int i = 0; i < 3; ++i) pthread_create(&threads[n++], NULL, poller, (void*)(uintptr_t)(i + 1));
    pthread_create(&threads[n++], NULL, forker, NULL);
    for (int i = 0; i < 2; ++i) pthread_create(&threads[n++], NULL, namespaces, (void*)(uintptr_t)(i + 1));
    for (int i = 0; i < 2; ++i) pthread_create(&threads[n++], NULL, memfds, (void*)(uintptr_t)(i + 1));
    for (int s = 0; s < seconds; ++s) {
        sleep(1);
        if (s % 10 == 9) {
            printf("PIPE_POLL_STRESS_PROGRESS t=%d poll=%ld create=%ld emfile=%ld send=%ld recv=%ld trunc=%ld fork=%ld close=%ld\n",
                   s + 1, counters[0], counters[1], counters[2], counters[3], counters[4], counters[5], counters[6], counters[7]);
            fflush(stdout);
        }
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < n; ++i) pthread_join(threads[i], NULL);
    unlink(fifo_path); unlink(socket_path);
    printf("PIPE_POLL_STRESS_OK poll=%ld create=%ld emfile=%ld send=%ld recv=%ld trunc=%ld fork=%ld close=%ld\n",
           counters[0], counters[1], counters[2], counters[3], counters[4], counters[5], counters[6], counters[7]);
    fflush(stdout);
    return 0;
}

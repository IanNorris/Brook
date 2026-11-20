/* Local policy regression for a disposable guarded guest with no deployment key. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "BRIDGE_LOCAL_POLICY_FAIL line=%d check=%s errno=%d\n", \
                __LINE__, #condition, errno); \
        _exit(2); \
    } \
} while (0)

enum { DEBUG_PORT = 1234, LAST_RESERVED_PORT = 1237, REQUEST_TIMEOUT_MS = 5000 };

static struct sockaddr_in address(uint32_t ip, unsigned port)
{
    return (struct sockaddr_in) { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {ip} };
}

static uint32_t own_ipv4(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    for (int index = 1; index <= 16; ++index) {
        struct ifreq entry = { .ifr_ifindex = index };
        if (ioctl(fd, SIOCGIFNAME, &entry) || ioctl(fd, SIOCGIFADDR, &entry)) continue;
        struct sockaddr_in local;
        memcpy(&local, &entry.ifr_addr, sizeof(local));
        const uint32_t ip = local.sin_addr.s_addr;
        if (local.sin_family == AF_INET && ip && (ntohl(ip) >> 24) != 127) {
            CHECK(close(fd) == 0);
            return ip;
        }
    }
    CHECK(close(fd) == 0);
    return 0;
}

static int64_t milliseconds(void)
{
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ready(int fd, short event, int64_t deadline)
{
    struct pollfd query = { .fd = fd, .events = event };
    for (;;) {
        int64_t remaining = deadline - milliseconds();
        CHECK(remaining > 0);
        int done = poll(&query, 1, (int)remaining);
        if (done < 0 && errno == EINTR) continue;
        CHECK(done == 1 && (query.revents & (event | POLLERR | POLLHUP)));
        return;
    }
}

static int connect_debug(uint32_t ip, int64_t deadline)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    struct sockaddr_in destination = address(ip, DEBUG_PORT);
    int connected = connect(fd, (struct sockaddr *)&destination, sizeof(destination));
    if (connected != 0) {
        CHECK(errno == EINPROGRESS);
        ready(fd, POLLOUT, deadline);
        int error = 0;
        socklen_t bytes = sizeof(error);
        CHECK(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &bytes) == 0);
        CHECK(error == 0);
    }
    return fd;
}

static void request(uint32_t ip, const char *command, const char *response_field)
{
    int64_t deadline = milliseconds() + REQUEST_TIMEOUT_MS;
    int fd = connect_debug(ip, deadline);
    char message[96];
    int length = snprintf(message, sizeof(message), "@1 %s\n", command);
    CHECK(length > 0 && (size_t)length < sizeof(message));
    for (size_t sent = 0; sent < (size_t)length;) {
        ready(fd, POLLOUT, deadline);
        ssize_t count = send(fd, message + sent, (size_t)length - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        CHECK(count > 0);
        sent += (size_t)count;
    }

    static const char begin_marker[] = "BRIDGE_BEGIN 1\n";
    static const char end_marker[] = "BRIDGE_END 1\n";
    char response[4096];
    for (size_t used = 0; used < sizeof(response) - 1;) {
        ready(fd, POLLIN, deadline);
        ssize_t count = recv(fd, response + used, sizeof(response) - used - 1, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        CHECK(count > 0);
        used += (size_t)count;
        response[used] = '\0';
        char *begin = strstr(response, begin_marker);
        char *end = begin ? strstr(begin + sizeof(begin_marker) - 1, end_marker) : NULL;
        if (!end) continue;
        *end = '\0';
        CHECK(strstr(begin + sizeof(begin_marker) - 1, response_field) != NULL);
        CHECK(close(fd) == 0);
        return;
    }
    CHECK(0 && "bounded framed response ending missing");
}

static void check_local_bridge(uint32_t ip)
{
    request(ip, "bridge hardware caps", "\"hardware_api\":1");
    // Read-only diagnostics that are deliberately outside the local allowlist.
    request(ip, "bridge stats", "{\"bridge_error\":\"local connections are read-only\"}");
}

static int64_t check_idle_local_deadline(void)
{
    const int64_t started = milliseconds();
    const int64_t deadline = started + 2000;
    int fd = connect_debug(htonl(INADDR_LOOPBACK), deadline);
    static const char banner[] = "=== Brook OS Debug Console ===\r\n";
    char received[sizeof(banner)];
    size_t used = 0;
    for (;;) {
        ready(fd, POLLIN, deadline);
        ssize_t count = recv(fd, received + used, sizeof(received) - used, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count == 0 || (count < 0 && errno == ECONNRESET)) break;
        CHECK(count > 0);
        used += (size_t)count;
        CHECK(used < sizeof(received));
    }
    CHECK(used == sizeof(banner) - 1 && !memcmp(received, banner, used));
    CHECK(close(fd) == 0);
    const int64_t elapsed = milliseconds() - started;
    CHECK(elapsed >= 0 && elapsed <= 2000);
    return elapsed;
}

static int bind_socket(int fd, uint32_t ip, unsigned port)
{
    struct sockaddr_in local = address(ip, port);
    return bind(fd, (struct sockaddr *)&local, sizeof(local));
}

static void check_bind_addresses(uint32_t local)
{
    // RFC 5737 documentation address, outside this guest's interfaces.
    const uint32_t nonlocal = htonl(0xc0000201u);
    const int types[] = {SOCK_STREAM, SOCK_DGRAM};
    for (size_t index = 0; index < sizeof(types) / sizeof(types[0]); ++index) {
        int fd = socket(AF_INET, types[index], 0);
        CHECK(fd >= 0 && bind_socket(fd, nonlocal, 0) == -1 && errno == EADDRNOTAVAIL);
        CHECK(bind_socket(fd, local, 0) == 0);
        CHECK(close(fd) == 0);
    }
}

static void check_reserved_tcp_denials(uint32_t local)
{
    const uint32_t addresses[] = {htonl(INADDR_ANY), htonl(INADDR_LOOPBACK), local};
    for (unsigned port = DEBUG_PORT; port <= LAST_RESERVED_PORT; ++port) {
        for (size_t which = 0; which < sizeof(addresses) / sizeof(addresses[0]); ++which) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            CHECK(fd >= 0);
            CHECK(bind_socket(fd, addresses[which], port) == -1 && errno == EACCES);
            CHECK(close(fd) == 0);
        }
    }
    int control = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(control >= 0 && bind_socket(control, htonl(INADDR_LOOPBACK), 0) == 0);
    CHECK(close(control) == 0);
}

static void check_free_ports(int type, uint32_t scope_address, unsigned first)
{
    for (unsigned port = first; port <= LAST_RESERVED_PORT; ++port) {
        int fd = socket(AF_INET, type, 0);
        CHECK(fd >= 0 && bind_socket(fd, scope_address, port) == 0);
        if (type == SOCK_STREAM) CHECK(listen(fd, 1) == 0);
        int conflict = socket(AF_INET, type, 0);
        CHECK(conflict >= 0);
        CHECK(bind_socket(conflict, htonl(INADDR_LOOPBACK), port) == -1 && errno == EADDRINUSE);
        CHECK(close(conflict) == 0 && close(fd) == 0);
    }
}

static void check_initial_root(void)
{
    // The existing kernel TCP listener still owns its initial-namespace port.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK(bind_socket(fd, htonl(INADDR_ANY), DEBUG_PORT) == -1 && errno == EADDRINUSE);
    CHECK(close(fd) == 0);
    check_free_ports(SOCK_STREAM, htonl(INADDR_ANY), DEBUG_PORT + 1);
}

static pid_t clone_private_root(unsigned long scope)
{
    pid_t child = syscall(SYS_clone, CLONE_NEWUSER | scope | SIGCHLD, 0UL, 0UL, 0UL, 0UL);
    CHECK(child >= 0);
    if (child == 0) {
        int map = open("/proc/self/uid_map", O_WRONLY);
        const char uid[] = "0 1000 1\n";
        CHECK(map >= 0 && write(map, uid, sizeof(uid) - 1) == sizeof(uid) - 1 && close(map) == 0);
        CHECK(geteuid() == 0);
    }
    return child;
}

static void wait_for_child(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && !strcmp(argv[1], "--disposable-fixture") && geteuid() == 0);
    const uint32_t local = own_ipv4();
    CHECK(local != 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setresgid(1000, 1000, 1000) == 0 && setresuid(1000, 1000, 1000) == 0);
        CHECK(geteuid() == 1000);
        check_local_bridge(htonl(INADDR_LOOPBACK));
        fprintf(stderr, "BRIDGE_LOCAL_LOOPBACK_PASS\n");
        check_local_bridge(local);
        fprintf(stderr, "BRIDGE_LOCAL_INTERFACE_PASS\n");
        const int64_t idle = check_idle_local_deadline();
        fprintf(stderr, "BRIDGE_LOCAL_IDLE_DEADLINE_PASS elapsed_ms=%lld\n", (long long)idle);
        check_bind_addresses(local);
        fprintf(stderr, "BRIDGE_LOCAL_BIND_ADDRESS_POLICY_PASS\n");
        check_reserved_tcp_denials(local);
        check_free_ports(SOCK_DGRAM, htonl(INADDR_ANY), DEBUG_PORT);
        fprintf(stderr, "BRIDGE_LOCAL_DESKTOP_BIND_POLICY_PASS\n");

        pid_t namespaced = clone_private_root(CLONE_NEWNS);
        if (!namespaced) {
            check_reserved_tcp_denials(local);
            check_free_ports(SOCK_DGRAM, htonl(INADDR_ANY), DEBUG_PORT);
            fprintf(stderr, "BRIDGE_LOCAL_USER_NAMESPACE_BIND_POLICY_PASS\n");
            _exit(0);
        }
        wait_for_child(namespaced);

        pid_t private_net = clone_private_root(CLONE_NEWNET);
        if (!private_net) {
            CHECK(own_ipv4() == 0);
            check_free_ports(SOCK_STREAM, htonl(INADDR_LOOPBACK), DEBUG_PORT);
            check_free_ports(SOCK_DGRAM, htonl(INADDR_LOOPBACK), DEBUG_PORT);
            fprintf(stderr, "BRIDGE_LOCAL_NET_NAMESPACE_BIND_POLICY_PASS\n");
            _exit(0);
        }
        wait_for_child(private_net);
        _exit(0);
    }
    wait_for_child(child);
    check_initial_root();
    fprintf(stderr, "BRIDGE_LOCAL_ROOT_BIND_PASS\nBRIDGE_LOCAL_POLICY_PASS\n");
    return 0;
}

#pragma once
/* True when a server is accepting connections on this Unix socket path. A
 * refused or missing path is stale and may be replaced. */
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static int wayland_socket_live(const struct sockaddr_un *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    int live = connect(fd, (const struct sockaddr *)path, sizeof(*path)) == 0;
    close(fd);
    return live;
}

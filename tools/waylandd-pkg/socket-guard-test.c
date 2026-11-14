/* A second waylandd must not replace a live socket, but may replace a stale one. */
#include "socket-guard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    char dir[] = "/tmp/brook-socket-guard-XXXXXX";
    assert(mkdtemp(dir));
    struct sockaddr_un a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/wayland-0", dir);
    assert(!wayland_socket_live(&a));                    /* missing path */
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(server >= 0 && !bind(server, (struct sockaddr *)&a, sizeof(a)) && !listen(server, 4));
    assert(wayland_socket_live(&a));                     /* live server */
    close(server);
    assert(!wayland_socket_live(&a));                    /* stale path left behind */
    unlink(a.sun_path); rmdir(dir);
    puts("socket-guard-test: ok");
    return 0;
}

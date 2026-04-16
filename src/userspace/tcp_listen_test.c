// tcp_listen_test.c — Test TCP listen/accept in Brook OS.
// Forks: parent listens+accepts, child connects+sends data.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#define TEST_PORT 7777
#define TEST_MSG  "Hello from client!"
#define RESP_MSG  "Hello from server!"

static int failures = 0;

static void check(int cond, const char* name)
{
    if (cond)
        printf("  [OK] %s\n", name);
    else {
        printf("  [FAIL] %s\n", name);
        failures++;
    }
}

static void test_listen_accept(void)
{
    // Create server socket
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    check(srv >= 0, "server socket created");
    if (srv < 0) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    int ret = bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    check(ret == 0, "bind");
    if (ret != 0) { close(srv); return; }

    ret = listen(srv, 5);
    check(ret == 0, "listen");
    if (ret != 0) { close(srv); return; }

    pid_t pid = fork();
    if (pid == 0)
    {
        // Child: connect to server
        close(srv);

        // Brief delay to let parent call accept
        for (volatile int i = 0; i < 500000; i++) {}

        int cli = socket(AF_INET, SOCK_STREAM, 0);
        if (cli < 0) _exit(1);

        struct sockaddr_in srvaddr;
        memset(&srvaddr, 0, sizeof(srvaddr));
        srvaddr.sin_family = AF_INET;
        srvaddr.sin_port = htons(TEST_PORT);
        srvaddr.sin_addr.s_addr = inet_addr("10.0.2.15"); // QEMU guest IP

        ret = connect(cli, (struct sockaddr*)&srvaddr, sizeof(srvaddr));
        if (ret != 0) _exit(2);

        // Send test message
        write(cli, TEST_MSG, strlen(TEST_MSG));

        // Read response
        char buf[64] = {};
        int n = read(cli, buf, sizeof(buf) - 1);
        if (n > 0) buf[n] = '\0';

        close(cli);
        _exit((strcmp(buf, RESP_MSG) == 0) ? 0 : 3);
    }
    else
    {
        // Parent: accept connection
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof(peer);
        int conn = accept(srv, (struct sockaddr*)&peer, &peerlen);
        check(conn >= 0, "accept returned fd");

        if (conn >= 0)
        {
            // Read client message
            char buf[64] = {};
            int n = read(conn, buf, sizeof(buf) - 1);
            if (n > 0) buf[n] = '\0';
            check(strcmp(buf, TEST_MSG) == 0, "received client message");

            // Send response
            write(conn, RESP_MSG, strlen(RESP_MSG));

            close(conn);
        }

        // Wait for child
        int status = 0;
        waitpid(pid, &status, 0);
        int child_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        check(child_exit == 0, "child received server response");

        close(srv);
    }
}

int main(void)
{
    printf("=== Brook TCP Listen/Accept Test ===\n");
    test_listen_accept();
    printf("\nResults: %d passed, %d failed\n",
           failures == 0 ? 6 : (6 - failures), failures);
    return failures ? 1 : 0;
}

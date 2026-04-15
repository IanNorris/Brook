/* cow_test.c — Tests Copy-on-Write fork for Brook OS.
 *
 * Build: musl-gcc -static -o cow_test cow_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int passed = 0, failed = 0;
static void ok(const char *name) { printf("  [OK] %s\n", name); passed++; }
static void fail(const char *name, const char *reason) {
    printf("  [FAIL] %s: %s\n", name, reason); failed++;
}

/* Test 1: basic fork — child exits, parent waits */
static void test_basic_fork(void) {
    pid_t pid = fork();
    if (pid < 0) { fail("basic_fork", "fork() failed"); return; }
    if (pid == 0) { _exit(42); }
    int status = 0;
    pid_t ret = waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 42)
        ok("basic_fork");
    else {
        printf("  [DEBUG] basic_fork: waitpid ret=%d, status=0x%x, WIFEXITED=%d, WEXITSTATUS=%d\n",
               ret, status, WIFEXITED(status), WEXITSTATUS(status));
        fail("basic_fork", "wrong exit status");
    }
}

/* Test 2: COW isolation — child writes don't affect parent */
static void test_cow_isolation(void) {
    volatile int shared_val = 0xDEAD;
    pid_t pid = fork();
    if (pid < 0) { fail("cow_isolation", "fork() failed"); return; }
    if (pid == 0) {
        /* Child modifies the variable */
        shared_val = 0xBEEF;
        /* Verify child sees its own write */
        _exit(shared_val == 0xBEEF ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* Parent should still see original value */
    if (shared_val != 0xDEAD) {
        fail("cow_isolation", "parent value corrupted by child write");
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("cow_isolation", "child didn't see its own write");
        return;
    }
    ok("cow_isolation");
}

/* Test 3: COW with heap — child writes to heap don't affect parent */
static void test_cow_heap(void) {
    char *buf = malloc(4096);
    if (!buf) { fail("cow_heap", "malloc failed"); return; }
    memset(buf, 'A', 4096);

    pid_t pid = fork();
    if (pid < 0) { fail("cow_heap", "fork() failed"); free(buf); return; }
    if (pid == 0) {
        /* Child fills buffer with 'B' */
        memset(buf, 'B', 4096);
        /* Verify child sees 'B' */
        int ok = 1;
        for (int i = 0; i < 4096; i++) {
            if (buf[i] != 'B') { ok = 0; break; }
        }
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* Parent should still see 'A' */
    int parent_ok = 1;
    for (int i = 0; i < 4096; i++) {
        if (buf[i] != 'A') { parent_ok = 0; break; }
    }
    free(buf);
    if (!parent_ok) { fail("cow_heap", "parent heap corrupted"); return; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("cow_heap", "child heap write failed"); return;
    }
    ok("cow_heap");
}

/* Test 4: multiple forks — each child gets independent copy */
static void test_multi_fork(void) {
    volatile int counter = 100;
    pid_t pids[4];
    for (int i = 0; i < 4; i++) {
        pids[i] = fork();
        if (pids[i] < 0) { fail("multi_fork", "fork() failed"); return; }
        if (pids[i] == 0) {
            counter += (i + 1) * 10;
            int expected = 100 + (i + 1) * 10;
            _exit(counter == expected ? 0 : 1);
        }
    }
    int all_ok = 1;
    for (int i = 0; i < 4; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            all_ok = 0;
    }
    if (counter != 100) {
        fail("multi_fork", "parent counter changed");
        return;
    }
    if (!all_ok) {
        fail("multi_fork", "a child saw wrong value");
        return;
    }
    ok("multi_fork");
}

/* Test 5: large write after fork — triggers multiple COW faults */
static void test_cow_large(void) {
    size_t sz = 64 * 1024; /* 64KB = 16 pages */
    char *buf = malloc(sz);
    if (!buf) { fail("cow_large", "malloc failed"); return; }
    memset(buf, 'X', sz);

    pid_t pid = fork();
    if (pid < 0) { fail("cow_large", "fork() failed"); free(buf); return; }
    if (pid == 0) {
        /* Child writes every page */
        for (size_t i = 0; i < sz; i += 4096)
            buf[i] = 'Y';
        /* Verify */
        int ok = 1;
        for (size_t i = 0; i < sz; i += 4096)
            if (buf[i] != 'Y') { ok = 0; break; }
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    /* Parent should still see 'X' everywhere */
    int parent_ok = 1;
    for (size_t i = 0; i < sz; i += 4096)
        if (buf[i] != 'X') { parent_ok = 0; break; }
    free(buf);
    if (!parent_ok) { fail("cow_large", "parent pages corrupted"); return; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("cow_large", "child page writes failed"); return;
    }
    ok("cow_large");
}

int main(void) {
    printf("=== Brook COW Fork Test ===\n");

    test_basic_fork();
    test_cow_isolation();
    test_cow_heap();
    test_multi_fork();
    test_cow_large();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

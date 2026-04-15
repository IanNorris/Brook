/* thread_test.c — Basic pthread test for Brook OS threading.
 *
 * Tests:
 *  1. pthread_create + pthread_join (basic thread lifecycle)
 *  2. Multiple threads running concurrently
 *  3. Thread return values
 *  4. Mutex synchronization
 *
 * Build: musl-gcc -static -o thread_test thread_test.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define NUM_THREADS 4
#define ITERATIONS  1000

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_counter = 0;

static void print_ok(const char *name) {
    printf("  [OK] %s\n", name);
}

static void print_fail(const char *name, const char *reason) {
    printf("  [FAIL] %s: %s\n", name, reason);
}

/* ---------- Test 1: basic create + join ---------- */

static void *simple_thread(void *arg) {
    int val = *(int *)arg;
    return (void *)(long)(val * 2);
}

static int test_basic_join(void) {
    int input = 21;
    pthread_t t;
    void *retval;

    if (pthread_create(&t, NULL, simple_thread, &input) != 0) {
        print_fail("basic_join", "pthread_create failed");
        return 1;
    }
    if (pthread_join(t, &retval) != 0) {
        print_fail("basic_join", "pthread_join failed");
        return 1;
    }
    if ((long)retval != 42) {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected 42, got %ld", (long)retval);
        print_fail("basic_join", buf);
        return 1;
    }
    print_ok("basic_join");
    return 0;
}

/* ---------- Test 2: gettid != getpid for threads ---------- */

static pid_t child_tid;
static pid_t child_pid;

static void *tid_thread(void *arg) {
    (void)arg;
    child_tid = (pid_t)syscall(SYS_gettid);
    child_pid = getpid();
    return NULL;
}

static int test_tid(void) {
    pthread_t t;
    pid_t parent_pid = getpid();

    if (pthread_create(&t, NULL, tid_thread, NULL) != 0) {
        print_fail("tid_check", "pthread_create failed");
        return 1;
    }
    pthread_join(t, NULL);

    if (child_pid != parent_pid) {
        char buf[128];
        snprintf(buf, sizeof(buf), "thread getpid=%d, parent=%d (should match)",
                 child_pid, parent_pid);
        print_fail("tid_check", buf);
        return 1;
    }
    if (child_tid == parent_pid) {
        /* TID should differ from PID (unless single-threaded, but we're not) */
        print_fail("tid_check", "thread TID equals main PID (no unique TID?)");
        return 1;
    }
    print_ok("tid_check");
    return 0;
}

/* ---------- Test 3: mutex + multiple threads ---------- */

static void *counter_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&g_mutex);
        g_counter++;
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

static int test_mutex(void) {
    pthread_t threads[NUM_THREADS];
    g_counter = 0;

    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, counter_thread, NULL) != 0) {
            print_fail("mutex_counter", "pthread_create failed");
            return 1;
        }
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    int expected = NUM_THREADS * ITERATIONS;
    if (g_counter != expected) {
        char buf[128];
        snprintf(buf, sizeof(buf), "counter=%d, expected=%d (race condition?)",
                 g_counter, expected);
        print_fail("mutex_counter", buf);
        return 1;
    }
    print_ok("mutex_counter");
    return 0;
}

/* ---------- Test 4: multiple joins with return values ---------- */

static void *return_thread(void *arg) {
    long id = (long)arg;
    return (void *)(id * id);
}

static int test_multi_return(void) {
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, return_thread, (void *)(long)i) != 0) {
            print_fail("multi_return", "pthread_create failed");
            return 1;
        }
    }

    int fail = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        void *retval;
        pthread_join(threads[i], &retval);
        long expected = (long)i * (long)i;
        if ((long)retval != expected) {
            char buf[128];
            snprintf(buf, sizeof(buf), "thread %d returned %ld, expected %ld",
                     i, (long)retval, expected);
            print_fail("multi_return", buf);
            fail = 1;
        }
    }
    if (!fail)
        print_ok("multi_return");
    return fail;
}

/* ---------- main ---------- */

int main(void) {
    printf("=== Brook threading test ===\n");
    printf("PID=%d TID=%ld\n", getpid(), syscall(SYS_gettid));

    int failures = 0;
    failures += test_basic_join();
    failures += test_tid();
    failures += test_mutex();
    failures += test_multi_return();

    printf("=== %d test(s) failed ===\n", failures);
    return failures ? 1 : 0;
}

/* Run on Linux and Brook: MADV_FREE is a lazy hint for private anonymous pages. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *p = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    assert(madvise(p + page, page, MADV_FREE) == 0); /* untouched */
    assert(mprotect(p + page, page, PROT_NONE) == 0);
    assert(madvise(p + page, page, MADV_FREE) == 0);
    assert(mprotect(p + page, page, PROT_READ | PROT_WRITE) == 0);
    p[0] = 0x42;
    assert(madvise(p, page, MADV_FREE) == 0);
    /* A subsequent write cancels any pending reclaim for this page. */
    p[0] = 0x73;
    assert(p[0] == 0x73);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { p[0] = 0x22; _exit(0); }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(p[0] == 0x73);
    errno = 0;
    assert(madvise(p + 1, page, MADV_FREE) == -1 && errno == EINVAL);
    assert(munmap(p + page, page) == 0);
    errno = 0;
    assert(madvise(p, 2 * page, MADV_FREE) == -1 && errno == ENOMEM);
    errno = 0;
    assert(madvise(p, page + 1, MADV_FREE) == -1 && errno == ENOMEM);
    assert(munmap(p, page) == 0);
    p = mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    errno = 0;
    assert(madvise(p, page, MADV_FREE) == -1 && errno == EINVAL);
    assert(munmap(p, page) == 0);
    puts("MADV_FREE_PASS");
    return 0;
}

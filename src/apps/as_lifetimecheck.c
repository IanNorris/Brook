// Guest regression: graphics backing survives fork, and window cleanup after
// exec uses the original address space rather than removing a new mapping.
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct window { uint32_t id, stride; uint64_t address; };
static void fail(const char* operation) { perror(operation); exit(1); }
static void create(struct window* w) {
    if (syscall(506, 240L, 160L, "AS lifetime", w, 0L) < 0) fail("create");
    if (!w->id || !w->address) fail("empty window");
}
static void resize(struct window* w, unsigned width, unsigned height) {
    if (syscall(511, (long)w->id, (long)width, (long)height, w) < 0) fail("resize");
    *(volatile uint32_t*)(uintptr_t)w->address = 0xff123456;
}
int main(int argc, char** argv) {
    if (argc == 4) {
        unsigned id = strtoul(argv[2], 0, 10);
        uintptr_t address = strtoull(argv[3], 0, 16);
        volatile uint32_t* p = mmap((void*)address, 4096, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
        if (p == MAP_FAILED || (uintptr_t)p != address) fail("replacement mmap");
        *p = 0x12345678;
        if (syscall(507, (long)id) < 0) fail("destroy old-AS window");
        if (*p != 0x12345678) fail("replacement clobbered");
        *p = 0x87654321;
        puts("ASLIFETIMECHECK PASS resize/fork-backing/exec-window-identity");
        return 0;
    }
    // Legacy compositor allocation list, separate from the native-window API.
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) fail("fb open");
    struct fb_var_screeninfo info;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &info)) fail("fb get");
    info.xres = 320; info.yres = 200;
    if (ioctl(fb, FBIOPUT_VSCREENINFO, &info)) fail("fb resize 1");
    void* legacy = mmap(0, 320*200*4, PROT_READ|PROT_WRITE, MAP_SHARED, fb, 0);
    if (legacy == MAP_FAILED) fail("fb mmap");
    *(volatile uint32_t*)legacy = 0xff998877;
    info.xres = 400; info.yres = 240;
    if (ioctl(fb, FBIOPUT_VSCREENINFO, &info)) fail("fb resize 2");
    if (*(volatile uint32_t*)legacy != 0xff998877) fail("old legacy backing");
    close(fb);
    struct window w;
    create(&w); resize(&w, 320, 200); resize(&w, 400, 240);
    int pipefd[2]; if (pipe(pipefd)) fail("pipe");
    pid_t child = fork(); if (child < 0) fail("fork");
    if (!child) {
        close(pipefd[1]); char token;
        if (read(pipefd[0], &token, 1) != 1) _exit(2);
        volatile uint32_t* p = (void*)(uintptr_t)w.address;
        if (*p != 0xff123456) _exit(3);
        *p = 0xffabcdef;
        _exit(0);
    }
    close(pipefd[0]);
    if (syscall(507, (long)w.id) < 0) fail("destroy parent window");
    usleep(100000); // Stress the deferred retirement window.
    if (write(pipefd[1], "x", 1) != 1) fail("release child");
    close(pipefd[1]);
    int status; if (waitpid(child, &status, 0) != child || status) fail("child backing");
    create(&w); resize(&w, 320, 200); resize(&w, 400, 240);
    char id[24], address[24];
    snprintf(id, sizeof(id), "%u", w.id);
    snprintf(address, sizeof(address), "%llx", (unsigned long long)w.address);
    execl(argv[0], argv[0], "after-exec", id, address, (char*)0);
    fail("exec");
}

/* Submit distinct user-window frames through the ordinary Brook WM ABI. */
#include <stdint.h>

enum {
    SYS_WRITE = 1,
    SYS_NANOSLEEP = 35,
    SYS_EXIT = 60,
    SYS_GETUID = 102,
    WM_CREATE_WINDOW = 506,
    WM_DESTROY_WINDOW = 507,
    WM_SIGNAL_DIRTY = 508,
    WM_NATIVE_APPLICATION = 4,
};

struct WindowBuffer {
    uint32_t window;
    uint32_t stride;
    uint64_t pixels;
};

static long call5(long n, long a, long b, long c, long d, long e)
{
    register long fourth __asm__("r10") = d;
    register long fifth __asm__("r8") = e;
    long result;
    __asm__ volatile("syscall" : "=a"(result)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(fourth), "r"(fifth)
                     : "rcx", "r11", "memory");
    return result;
}

/* A raw ELF entry has no return address on the initial process stack. */
__attribute__((force_align_arg_pointer, noreturn)) void _start(void)
{
    static const char title[] = "Guard health fixture";
    static const char pass[] = "GUARD_NATIVE_FRAMES_PASS\n";
    static const char fail[] = "GUARD_NATIVE_FRAMES_FAIL\n";
    static const uint32_t colors[] = {
        0xffd61f35, 0xff2b64d2, 0xff117536, 0xffe99123,
    };
    static const long interval[] = {0, 150000000};
    enum { Width = 96, Height = 64 };
    struct WindowBuffer out = {0};

    int ok = call5(SYS_GETUID, 0, 0, 0, 0, 0) == 1000 &&
             !call5(WM_CREATE_WINDOW, Width, Height, (long)title, (long)&out,
                    WM_NATIVE_APPLICATION) &&
             out.window && out.pixels && out.stride >= Width;
    volatile uint32_t* pixels = (volatile uint32_t*)(uintptr_t)out.pixels;
    for (unsigned frame = 0; ok && frame < sizeof(colors) / sizeof(*colors); ++frame) {
        for (unsigned y = 0; y < Height; ++y)
            for (unsigned x = 0; x < Width; ++x)
                pixels[y * out.stride + x] = colors[frame];
        ok = !call5(WM_SIGNAL_DIRTY, out.window, 0, 0, 0, 0) &&
             !call5(SYS_NANOSLEEP, (long)interval, 0, 0, 0, 0);
    }

    if (out.window && call5(WM_DESTROY_WINDOW, out.window, 0, 0, 0, 0))
        ok = 0;
    call5(SYS_WRITE, 1, (long)(ok ? pass : fail), ok ? sizeof(pass) - 1 : sizeof(fail) - 1, 0, 0);
    call5(SYS_EXIT, ok ? 0 : 1, 0, 0, 0, 0);
    __builtin_unreachable();
}

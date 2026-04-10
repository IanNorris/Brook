#pragma once
// Minimal assert.h for freestanding kernel use.

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else

static inline void _assert_serial_putc(char c)
{
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)c), "Nd"((unsigned short)0x3F8));
}

static inline void _assert_serial_puts(const char* s)
{
    while (*s) _assert_serial_putc(*s++);
}

static inline void _assert_fail(const char* expr, const char* file, int line)
{
    _assert_serial_puts("ASSERT FAIL: ");
    _assert_serial_puts(expr);
    _assert_serial_putc(' ');
    _assert_serial_puts(file);
    _assert_serial_putc('\n');
    for (;;) __asm__ volatile("cli; hlt");
}

#define assert(expr) \
    ((expr) ? ((void)0) : _assert_fail(#expr, __FILE__, __LINE__))

#endif

#ifdef __cplusplus
}
#endif

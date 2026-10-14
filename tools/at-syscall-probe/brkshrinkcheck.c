#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Never call an allocator or stdio while borrowing the process break. The
   syscall wrapper, byte loops and raw fork/wait/exit need no heap storage. */
static uintptr_t set_break(uintptr_t address)
{
    return (uintptr_t)syscall(SYS_brk, address);
}

static void fill(volatile unsigned char *p, size_t count, unsigned char byte)
{
    for (size_t i = 0; i < count; ++i) p[i] = byte;
}

static int matches(volatile unsigned char *p, size_t count, unsigned char byte)
{
    for (size_t i = 0; i < count; ++i) if (p[i] != byte) return 0;
    return 1;
}

int main(void)
{
    uintptr_t original = set_break(0);
    uintptr_t page = (original + 4095) & ~(uintptr_t)4095;
    uintptr_t end = page + 4 * 4096;
    volatile unsigned char *bytes = (volatile unsigned char *)page;
    unsigned failures = 0;
    if (set_break(end) != end) return 2;

    fill(bytes, 4 * 4096, 0x6d);
    if (set_break(page + 123) != page + 123 || set_break(end) != end)
        failures |= 1;
    if (!matches(bytes, 4096, 0x6d)) failures |= 2;
    if (!matches(bytes + 4096, 3 * 4096, 0)) failures |= 4;

    fill(bytes, 4 * 4096, 0xa7);
    if (set_break(page + 4096) != page + 4096 || set_break(end) != end)
        failures |= 8;
    if (!matches(bytes, 4096, 0xa7)) failures |= 16;
    if (!matches(bytes + 4096, 3 * 4096, 0)) failures |= 32;

    /* Shrink a COW heap in the child: dropping its references must neither
       free nor zero the parent's frames. Its retained page still needs COW. */
    fill(bytes, 4 * 4096, 0x39);
    long child = syscall(SYS_fork);
    if (child == 0) {
        int bad = set_break(page + 123) != page + 123;
        bad |= set_break(end) != end;
        bad |= !matches(bytes, 4096, 0x39);
        bad |= !matches(bytes + 4096, 3 * 4096, 0);
        fill(bytes, 4 * 4096, 0xc2);
        syscall(SYS_exit, bad ? 1 : 0);
        __builtin_unreachable();
    }
    int status = 0;
    if (child < 0 || syscall(SYS_wait4, child, &status, 0, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) failures |= 64;
    if (!matches(bytes, 4 * 4096, 0x39)) failures |= 128;
    if (set_break(original) != original) failures |= 256;

    const char *labels[] = {
        "partial-page shrink/regrow succeeds", "live partial page preserved",
        "partial-page regrowth zeroes full pages", "aligned shrink/regrow succeeds",
        "retained full page preserved", "aligned regrowth zeroes full pages",
        "fork child regrowth zeroes its pages", "fork parent pages unchanged",
        "original break restored"
    };
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
        printf("BRKSHRINKCHECK %s %s\n", failures & (1u << i) ? "FAIL" : "PASS", labels[i]);
    printf("BRKSHRINKCHECK %s mask=0x%x\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

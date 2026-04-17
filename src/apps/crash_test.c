/* crash_test.c — Deliberate crash generator for testing Brook's panic infrastructure.
 *
 * Build: musl-gcc -static -o crash_test crash_test.c
 * Usage: crash_test <mode>
 *   null       — dereference NULL pointer (SIGSEGV / #PF)
 *   divzero    — integer divide by zero (#DE)
 *   stackoverflow — infinite recursion (stack overflow → #PF)
 *   gpf        — execute privileged instruction from userspace (#GP)
 *   ud         — execute undefined opcode (#UD)
 *   wild       — jump to wild address (0xDEADBEEF)
 *   writekernel — write to kernel address space (#PF / #GP)
 *   readkernel  — read from kernel address space (#PF)
 *   int3       — breakpoint trap (#BP)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static volatile int g_sink;

__attribute__((noinline))
static void crash_null_deref(void) {
    volatile int *p = (volatile int *)0;
    g_sink = *p;
}

__attribute__((noinline))
static void crash_div_zero(void) {
    volatile int a = 1;
    volatile int b = 0;
    g_sink = a / b;
}

/* Recursive function for stack overflow */
__attribute__((noinline))
static void crash_recurse(volatile int depth) {
    volatile char buf[4096]; /* eat stack fast */
    buf[0] = (char)depth;
    g_sink = buf[0];
    crash_recurse(depth + 1);
}

__attribute__((noinline))
static void crash_stackoverflow(void) {
    crash_recurse(0);
}

__attribute__((noinline))
static void crash_gpf(void) {
    /* HLT is ring-0 only — triggers #GP from userspace */
    __asm__ volatile("hlt");
}

__attribute__((noinline))
static void crash_ud(void) {
    /* UD2 — guaranteed undefined opcode */
    __asm__ volatile("ud2");
}

__attribute__((noinline))
static void crash_wild_jump(void) {
    void (*fn)(void) = (void (*)(void))0xDEADBEEF;
    fn();
}

__attribute__((noinline))
static void crash_write_kernel(void) {
    /* Write to a typical kernel address */
    volatile uint64_t *p = (volatile uint64_t *)0xFFFFFFFF80000000ULL;
    *p = 0x4242424242424242ULL;
}

__attribute__((noinline))
static void crash_read_kernel(void) {
    volatile uint64_t *p = (volatile uint64_t *)0xFFFFFFFF80000000ULL;
    g_sink = (int)*p;
}

__attribute__((noinline))
static void crash_int3(void) {
    __asm__ volatile("int3");
}

struct crash_mode {
    const char *name;
    void (*fn)(void);
    const char *desc;
};

static const struct crash_mode modes[] = {
    { "null",         crash_null_deref,   "NULL pointer dereference (#PF)" },
    { "divzero",      crash_div_zero,     "Integer divide by zero (#DE)" },
    { "stackoverflow",crash_stackoverflow,"Stack overflow via recursion (#PF)" },
    { "gpf",          crash_gpf,          "Privileged instruction from user (#GP)" },
    { "ud",           crash_ud,           "Undefined opcode (#UD)" },
    { "wild",         crash_wild_jump,    "Jump to wild address 0xDEADBEEF" },
    { "writekernel",  crash_write_kernel, "Write to kernel address (#PF/#GP)" },
    { "readkernel",   crash_read_kernel,  "Read from kernel address (#PF)" },
    { "int3",         crash_int3,         "Breakpoint trap (#BP)" },
    { NULL, NULL, NULL }
};

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("crash_test — deliberate crash generator\n\nUsage: crash_test <mode>\n\nModes:\n");
        for (int i = 0; modes[i].name; i++)
            printf("  %-16s %s\n", modes[i].name, modes[i].desc);
        return 1;
    }

    for (int i = 0; modes[i].name; i++) {
        if (strcmp(argv[1], modes[i].name) == 0) {
            printf("crash_test: triggering '%s' — %s\n", modes[i].name, modes[i].desc);
            modes[i].fn();
            printf("crash_test: ERROR — crash did not occur?!\n");
            return 2;
        }
    }

    printf("crash_test: unknown mode '%s'\n", argv[1]);
    return 1;
}

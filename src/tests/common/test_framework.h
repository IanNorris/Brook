#pragma once
#include "boot_protocol/boot_protocol.h"
#include "serial.h"

namespace brook {
namespace test {

// Filled by TEST_MAIN macro
static BootProtocol* g_protocol = nullptr;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void DrawFullScreen(const Framebuffer& fb, uint32_t colour) {
    uint32_t* pixels = reinterpret_cast<uint32_t*>(fb.physicalBase);
    uint32_t stride = fb.stride / 4;
    for (uint32_t y = 0; y < fb.height; y++)
        for (uint32_t x = 0; x < fb.width; x++)
            pixels[y * stride + x] = colour;
}

static void TestPass(const char* name) {
    SerialPrintf("[PASS] %s\n", name);
    if (g_protocol)
        DrawFullScreen(g_protocol->framebuffer, 0x00004400);  // dark green
    outb(0xf4, 0);  // QEMU isa-debug-exit: write 0 → exit code 1 (pass)
    for (;;) { __asm__ volatile("hlt"); }
}

static void TestFail(const char* name, const char* file, int line, const char* expr) {
    SerialPrintf("[FAIL] %s at %s:%d: %s\n", name, file, line, expr);
    if (g_protocol)
        DrawFullScreen(g_protocol->framebuffer, 0x00440000);  // dark red
    outb(0xf4, 1);  // QEMU isa-debug-exit: write 1 → exit code 3 (fail)
    for (;;) { __asm__ volatile("hlt"); }
}

} // namespace test
} // namespace brook

// Macros for use in test files
#define ASSERT_TRUE(expr) \
    do { if (!(expr)) brook::test::TestFail(g_test_name, __FILE__, __LINE__, #expr); } while(0)

#define ASSERT_FALSE(expr) \
    do { if (expr) brook::test::TestFail(g_test_name, __FILE__, __LINE__, "!" #expr); } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) brook::test::TestFail(g_test_name, __FILE__, __LINE__, #a " != " #b); } while(0)

#define ASSERT_NE(a, b) \
    do { if ((a) == (b)) brook::test::TestFail(g_test_name, __FILE__, __LINE__, #a " == " #b); } while(0)

// TEST_MAIN: define this in each test file. NAME is a string literal.
#define TEST_MAIN(NAME, body) \
    static const char* g_test_name = NAME; \
    extern "C" __attribute__((sysv_abi)) void KernelMain(brook::BootProtocol* bp) { \
        if (!bp || bp->magic != brook::BootProtocolMagic) { \
            brook::test::outb(0xf4, 1); \
            for (;;) { __asm__ volatile("hlt"); } \
        } \
        brook::test::g_protocol = bp; \
        brook::SerialInit(); \
        brook::SerialPrintf("=== TEST: %s ===\n", NAME); \
        body \
        brook::test::TestPass(NAME); \
    }

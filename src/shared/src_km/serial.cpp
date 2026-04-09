#include "serial.h"

namespace brook {

static constexpr uint16_t COM1_BASE = 0x3F8;
static constexpr uint16_t COM1_DATA = COM1_BASE + 0;
static constexpr uint16_t COM1_IER  = COM1_BASE + 1;
static constexpr uint16_t COM1_FCR  = COM1_BASE + 2;
static constexpr uint16_t COM1_LCR  = COM1_BASE + 3;
static constexpr uint16_t COM1_MCR  = COM1_BASE + 4;
static constexpr uint16_t COM1_LSR  = COM1_BASE + 5;

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void SerialInit()
{
    outb(COM1_IER, 0x00);   // Disable interrupts
    outb(COM1_LCR, 0x80);   // Enable DLAB
    outb(COM1_DATA, 0x01);  // Divisor low byte (115200 baud)
    outb(COM1_IER,  0x00);  // Divisor high byte
    outb(COM1_LCR, 0x03);   // Clear DLAB; 8N1
    outb(COM1_FCR, 0x00);   // Disable FIFO
    outb(COM1_MCR, 0x03);   // DTR + RTS
}

void SerialPutChar(char c)
{
    if (c == '\n') {
        // QEMU serial stdio expects CRLF
        while ((inb(COM1_LSR) & 0x20) == 0) {}
        outb(COM1_DATA, '\r');
    }
    while ((inb(COM1_LSR) & 0x20) == 0) {}
    outb(COM1_DATA, static_cast<uint8_t>(c));
}

void SerialPuts(const char* str)
{
    if (!str) return;
    while (*str) SerialPutChar(*str++);
}

// ---- Internal helpers for SerialPrintf ----

static void PrintUlong(unsigned long val)
{
    if (val == 0) { SerialPutChar('0'); return; }
    char buf[20];
    int i = 0;
    while (val > 0) {
        buf[i++] = static_cast<char>('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) SerialPutChar(buf[--i]);
}

static void PrintHex(unsigned long val)
{
    if (val == 0) { SerialPutChar('0'); return; }
    char buf[16];
    int i = 0;
    while (val > 0) {
        int nibble = static_cast<int>(val & 0xF);
        buf[i++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        val >>= 4;
    }
    while (i > 0) SerialPutChar(buf[--i]);
}

static void PrintPtr(unsigned long val)
{
    SerialPuts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        int nibble = static_cast<int>((val >> shift) & 0xF);
        SerialPutChar(static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10));
    }
}

void SerialVPrintf(const char* fmt, __builtin_va_list args)
{
    while (*fmt) {
        if (*fmt != '%') {
            SerialPutChar(*fmt++);
            continue;
        }
        fmt++;  // skip '%'
        if (*fmt == '\0') break;

        switch (*fmt) {
            case 's': {
                const char* s = __builtin_va_arg(args, const char*);
                SerialPuts(s ? s : "(null)");
                break;
            }
            case 'd': {
                int val = __builtin_va_arg(args, int);
                if (val < 0) {
                    SerialPutChar('-');
                    PrintUlong(static_cast<unsigned long>(-static_cast<long>(val)));
                } else {
                    PrintUlong(static_cast<unsigned long>(val));
                }
                break;
            }
            case 'u': {
                unsigned int val = __builtin_va_arg(args, unsigned int);
                PrintUlong(static_cast<unsigned long>(val));
                break;
            }
            case 'x': {
                unsigned int val = __builtin_va_arg(args, unsigned int);
                PrintHex(static_cast<unsigned long>(val));
                break;
            }
            case 'l': {
                fmt++;
                if (*fmt == 'u') {
                    PrintUlong(__builtin_va_arg(args, unsigned long));
                } else if (*fmt == 'x') {
                    PrintHex(__builtin_va_arg(args, unsigned long));
                } else if (*fmt == 'd') {
                    long val = __builtin_va_arg(args, long);
                    if (val < 0) { SerialPutChar('-'); PrintUlong(static_cast<unsigned long>(-val)); }
                    else PrintUlong(static_cast<unsigned long>(val));
                } else {
                    SerialPutChar('l');
                    SerialPutChar(*fmt);
                }
                break;
            }
            case 'p': {
                void* ptr = __builtin_va_arg(args, void*);
                PrintPtr(reinterpret_cast<unsigned long>(ptr));
                break;
            }
            case 'c': {
                int c = __builtin_va_arg(args, int);
                SerialPutChar(static_cast<char>(c));
                break;
            }
            case '%': {
                SerialPutChar('%');
                break;
            }
            default: {
                SerialPutChar('%');
                SerialPutChar(*fmt);
                break;
            }
        }
        fmt++;
    }
}

void SerialPrintf(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    SerialVPrintf(fmt, args);
    __builtin_va_end(args);
}

} // namespace brook

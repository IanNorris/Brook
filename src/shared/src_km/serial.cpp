#include "serial.h"

namespace brook {

// IRQ-safe ticket spinlock for serialising serial output across CPUs.
// Disables interrupts while held to prevent ISR-on-same-CPU deadlock.
struct SerialSpinlock {
    volatile uint32_t next;
    volatile uint32_t serving;
};
static SerialSpinlock g_serialLock = {0, 0};

// Per-CPU saved flags (max 64 CPUs). Nesting not supported — serial lock
// is never taken recursively.
static uint64_t g_serialSavedFlags[64];

static inline uint32_t SerialThisCpu()
{
    uint32_t id;
    __asm__ volatile("mov $1, %%eax; cpuid; shr $24, %%ebx; mov %%ebx, %0"
                     : "=r"(id) : : "eax","ebx","ecx","edx");
    return id < 64 ? id : 0;
}

static inline void SerialLockAcquire()
{
    // Save flags and disable interrupts BEFORE taking the ticket.
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    uint32_t ticket = __atomic_fetch_add(&g_serialLock.next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&g_serialLock.serving, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");

    g_serialSavedFlags[SerialThisCpu()] = flags;
}

static inline void SerialLockRelease()
{
    uint64_t flags = g_serialSavedFlags[SerialThisCpu()];
    __atomic_fetch_add(&g_serialLock.serving, 1, __ATOMIC_RELEASE);

    // Restore interrupt flag after releasing the lock.
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
}

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
    SerialLockAcquire();
    while (*str) SerialPutChar(*str++);
    SerialLockRelease();
}

// Unlocked puts — for use inside already-locked contexts (SerialVPrintf etc.).
static void SerialPutsRaw(const char* str)
{
    if (!str) return;
    while (*str) SerialPutChar(*str++);
}

void SerialLock()  { SerialLockAcquire(); }
void SerialUnlock() { SerialLockRelease(); }

// ---- Internal helper for SerialPrintf ----

static void PrintPtr(unsigned long val)
{
    SerialPutsRaw("0x");
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

        // Parse flags
        bool leftAlign = false;
        bool zeroPad   = false;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') leftAlign = true;
            if (*fmt == '0') zeroPad   = true;
            fmt++;
        }
        if (leftAlign) zeroPad = false;  // '-' overrides '0'

        // Parse width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == '\0') break;

        // Parse 'l' length modifier
        bool isLong = false;
        if (*fmt == 'l') { isLong = true; fmt++; }
        if (*fmt == '\0') break;

        // Temporary buffer for formatted number/string
        char buf[24];
        int len = 0;

        switch (*fmt) {
            case 's': {
                const char* s = __builtin_va_arg(args, const char*);
                if (!s) s = "(null)";
                // Count length
                const char* p = s;
                while (*p) { ++p; ++len; }
                // Pad right (left-align: print string then spaces)
                if (!leftAlign) { for (int i = len; i < width; ++i) SerialPutChar(' '); }
                SerialPutsRaw(s);
                if (leftAlign) { for (int i = len; i < width; ++i) SerialPutChar(' '); }
                break;
            }
            case 'd': {
                long val;
                if (isLong) val = __builtin_va_arg(args, long);
                else        val = static_cast<long>(__builtin_va_arg(args, int));
                bool neg = val < 0;
                unsigned long uval = neg ? static_cast<unsigned long>(-val) : static_cast<unsigned long>(val);
                len = 0;
                if (uval == 0) { buf[len++] = '0'; }
                else { while (uval > 0) { buf[len++] = static_cast<char>('0' + (uval % 10)); uval /= 10; } }
                int totalLen = len + (neg ? 1 : 0);
                if (!leftAlign && !zeroPad) { for (int i = totalLen; i < width; ++i) SerialPutChar(' '); }
                if (neg) SerialPutChar('-');
                if (!leftAlign && zeroPad) { for (int i = totalLen; i < width; ++i) SerialPutChar('0'); }
                while (len > 0) SerialPutChar(buf[--len]);
                if (leftAlign) { for (int i = totalLen; i < width; ++i) SerialPutChar(' '); }
                break;
            }
            case 'u': {
                unsigned long val;
                if (isLong) val = __builtin_va_arg(args, unsigned long);
                else        val = static_cast<unsigned long>(__builtin_va_arg(args, unsigned int));
                len = 0;
                if (val == 0) { buf[len++] = '0'; }
                else { while (val > 0) { buf[len++] = static_cast<char>('0' + (val % 10)); val /= 10; } }
                char padChar = zeroPad ? '0' : ' ';
                if (!leftAlign) { for (int i = len; i < width; ++i) SerialPutChar(padChar); }
                while (len > 0) SerialPutChar(buf[--len]);
                if (leftAlign) { for (int i = len; i < width; ++i) SerialPutChar(' '); }
                break;
            }
            case 'x': {
                unsigned long val;
                if (isLong) val = __builtin_va_arg(args, unsigned long);
                else        val = static_cast<unsigned long>(__builtin_va_arg(args, unsigned int));
                len = 0;
                if (val == 0) { buf[len++] = '0'; }
                else { while (val > 0) { int n = static_cast<int>(val & 0xF); buf[len++] = static_cast<char>(n < 10 ? '0' + n : 'a' + n - 10); val >>= 4; } }
                char padChar = zeroPad ? '0' : ' ';
                if (!leftAlign) { for (int i = len; i < width; ++i) SerialPutChar(padChar); }
                while (len > 0) SerialPutChar(buf[--len]);
                if (leftAlign) { for (int i = len; i < width; ++i) SerialPutChar(' '); }
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
                if (isLong) SerialPutChar('l');
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
    SerialLockAcquire();
    SerialVPrintf(fmt, args);
    SerialLockRelease();
    __builtin_va_end(args);
}

} // namespace brook

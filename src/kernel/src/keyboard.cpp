// keyboard.cpp — compiled with -mgeneral-regs-only (ISR file).
#include "keyboard.h"
#include "input.h"
#include "idt.h"
#include "apic.h"
#include "serial.h"
#include "tty.h"
#include "kprintf.h"

namespace brook {

// ---------------------------------------------------------------------------
// Port I/O
// ---------------------------------------------------------------------------

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

[[maybe_unused]]
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// ---------------------------------------------------------------------------
// Scan code set 1 → ASCII table
//
// Index = scan code (make code only, release codes have bit 7 set).
// 0 = no translation (modifier, unmapped, etc.)
// ---------------------------------------------------------------------------

static const char g_scancodeToAscii[128] = {
    /* 0x00 */ 0,
    /* 0x01 Esc      */ 0x1B,
    /* 0x02 1        */ '1',
    /* 0x03 2        */ '2',
    /* 0x04 3        */ '3',
    /* 0x05 4        */ '4',
    /* 0x06 5        */ '5',
    /* 0x07 6        */ '6',
    /* 0x08 7        */ '7',
    /* 0x09 8        */ '8',
    /* 0x0A 9        */ '9',
    /* 0x0B 0        */ '0',
    /* 0x0C -        */ '-',
    /* 0x0D =        */ '=',
    /* 0x0E Bksp     */ '\b',
    /* 0x0F Tab      */ '\t',
    /* 0x10 Q        */ 'q',
    /* 0x11 W        */ 'w',
    /* 0x12 E        */ 'e',
    /* 0x13 R        */ 'r',
    /* 0x14 T        */ 't',
    /* 0x15 Y        */ 'y',
    /* 0x16 U        */ 'u',
    /* 0x17 I        */ 'i',
    /* 0x18 O        */ 'o',
    /* 0x19 P        */ 'p',
    /* 0x1A [        */ '[',
    /* 0x1B ]        */ ']',
    /* 0x1C Enter    */ '\n',
    /* 0x1D L-Ctrl   */ 0,
    /* 0x1E A        */ 'a',
    /* 0x1F S        */ 's',
    /* 0x20 D        */ 'd',
    /* 0x21 F        */ 'f',
    /* 0x22 G        */ 'g',
    /* 0x23 H        */ 'h',
    /* 0x24 J        */ 'j',
    /* 0x25 K        */ 'k',
    /* 0x26 L        */ 'l',
    /* 0x27 ;        */ ';',
    /* 0x28 '        */ '\'',
    /* 0x29 `        */ '`',
    /* 0x2A L-Shift  */ 0,
    /* 0x2B \        */ '\\',
    /* 0x2C Z        */ 'z',
    /* 0x2D X        */ 'x',
    /* 0x2E C        */ 'c',
    /* 0x2F V        */ 'v',
    /* 0x30 B        */ 'b',
    /* 0x31 N        */ 'n',
    /* 0x32 M        */ 'm',
    /* 0x33 ,        */ ',',
    /* 0x34 .        */ '.',
    /* 0x35 /        */ '/',
    /* 0x36 R-Shift  */ 0,
    /* 0x37 *        */ '*',
    /* 0x38 L-Alt    */ 0,
    /* 0x39 Space    */ ' ',
    /* 0x3A Caps     */ 0,
    /* 0x3B F1       */ 0,
    /* 0x3C F2       */ 0,
    /* 0x3D F3       */ 0,
    /* 0x3E F4       */ 0,
    /* 0x3F F5       */ 0,
    /* 0x40 F6       */ 0,
    /* 0x41 F7       */ 0,
    /* 0x42 F8       */ 0,
    /* 0x43 F9       */ 0,
    /* 0x44 F10      */ 0,
    /* 0x45 NumLk    */ 0,
    /* 0x46 ScrlLk   */ 0,
    /* 0x47 KP7      */ '7',
    /* 0x48 KP8 / Up */ '8',
    /* 0x49 KP9      */ '9',
    /* 0x4A KP-      */ '-',
    /* 0x4B KP4      */ '4',
    /* 0x4C KP5      */ '5',
    /* 0x4D KP6      */ '6',
    /* 0x4E KP+      */ '+',
    /* 0x4F KP1      */ '1',
    /* 0x50 KP2/Dn   */ '2',
    /* 0x51 KP3      */ '3',
    /* 0x52 KP0      */ '0',
    /* 0x53 KP.      */ '.',
    /* 0x54          */ 0,
    /* 0x55          */ 0,
    /* 0x56          */ 0,
    /* 0x57 F11      */ 0,
    /* 0x58 F12      */ 0,
    /* 0x59–0x7F     */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static const char g_scancodeToAsciiShift[128] = {
    /* 0x00 */ 0,
    /* 0x01 Esc */ 0x1B,
    /* 0x02 1 */ '!',
    /* 0x03 2 */ '@',
    /* 0x04 3 */ '#',
    /* 0x05 4 */ '$',
    /* 0x06 5 */ '%',
    /* 0x07 6 */ '^',
    /* 0x08 7 */ '&',
    /* 0x09 8 */ '*',
    /* 0x0A 9 */ '(',
    /* 0x0B 0 */ ')',
    /* 0x0C - */ '_',
    /* 0x0D = */ '+',
    /* 0x0E Bksp */ '\b',
    /* 0x0F Tab  */ '\t',
    /* 0x10 */ 'Q',
    /* 0x11 */ 'W',
    /* 0x12 */ 'E',
    /* 0x13 */ 'R',
    /* 0x14 */ 'T',
    /* 0x15 */ 'Y',
    /* 0x16 */ 'U',
    /* 0x17 */ 'I',
    /* 0x18 */ 'O',
    /* 0x19 */ 'P',
    /* 0x1A */ '{',
    /* 0x1B */ '}',
    /* 0x1C */ '\n',
    /* 0x1D */ 0,
    /* 0x1E */ 'A',
    /* 0x1F */ 'S',
    /* 0x20 */ 'D',
    /* 0x21 */ 'F',
    /* 0x22 */ 'G',
    /* 0x23 */ 'H',
    /* 0x24 */ 'J',
    /* 0x25 */ 'K',
    /* 0x26 */ 'L',
    /* 0x27 */ ':',
    /* 0x28 */ '"',
    /* 0x29 */ '~',
    /* 0x2A */ 0,
    /* 0x2B */ '|',
    /* 0x2C */ 'Z',
    /* 0x2D */ 'X',
    /* 0x2E */ 'C',
    /* 0x2F */ 'V',
    /* 0x30 */ 'B',
    /* 0x31 */ 'N',
    /* 0x32 */ 'M',
    /* 0x33 */ '<',
    /* 0x34 */ '>',
    /* 0x35 */ '?',
    /* 0x36 */ 0,
    /* 0x37 */ '*',
    /* 0x38 */ 0,
    /* 0x39 */ ' ',
    /* 0x3A–0x7F */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

// ---------------------------------------------------------------------------
// Ring buffer (64 chars) — written by ISR, read by KbdGetChar/KbdPeekChar.
// ---------------------------------------------------------------------------

static constexpr int KBD_BUF_SIZE = 64;
static volatile char  g_kbdBuf[KBD_BUF_SIZE];
static volatile int   g_kbdHead = 0; // write index (ISR writes)
static volatile int   g_kbdTail = 0; // read  index (kernel reads)

// Input subsystem device — pushes raw scan code events for userspace consumers.
static InputDeviceOps g_kbdInputOps = { "ps2_kbd", nullptr };
static InputDevice    g_kbdInputDev = { &g_kbdInputOps, {}, 0, 0, nullptr };

static inline bool BufFull()  { return ((g_kbdHead + 1) % KBD_BUF_SIZE) == g_kbdTail; }
static inline bool BufEmpty() { return g_kbdHead == g_kbdTail; }

static void BufPush(char c)
{
    if (!BufFull())
    {
        g_kbdBuf[g_kbdHead] = c;
        g_kbdHead = (g_kbdHead + 1) % KBD_BUF_SIZE;
    }
}

static char BufPop()
{
    char c = g_kbdBuf[g_kbdTail];
    g_kbdTail = (g_kbdTail + 1) % KBD_BUF_SIZE;
    return c;
}

// ---------------------------------------------------------------------------
// Modifier state
// ---------------------------------------------------------------------------

static volatile bool g_shiftHeld = false;
static volatile bool g_capsLock  = false;

// ---------------------------------------------------------------------------
// IRQ1 interrupt handler
// ---------------------------------------------------------------------------

__attribute__((interrupt))
static void KbdIrqHandler(InterruptFrame* frame)
{
    (void)frame;

    uint8_t sc = inb(0x60); // read scan code from PS/2 data port

    bool release = (sc & 0x80) != 0;
    uint8_t key  = sc & 0x7F;

    // Build modifier bitmask for the input event.
    uint8_t mods = 0;
    if (g_shiftHeld) mods |= INPUT_MOD_LSHIFT;
    if (g_capsLock)  mods |= INPUT_MOD_CAPSLOCK;

    // Track modifier state.
    if (key == 0x2A || key == 0x36) // L-Shift or R-Shift
    {
        g_shiftHeld = !release;
    }
    else if (key == 0x3A && !release) // Caps Lock (toggle on press)
    {
        g_capsLock = !g_capsLock;
    }

    // Push raw scan code event to input subsystem (for userspace consumers like DOOM).
    // All keys (including modifiers) and both press/release are forwarded.
    {
        InputEvent ev;
        ev.type = release ? InputEventType::KeyRelease : InputEventType::KeyPress;
        ev.scanCode = key;
        ev.modifiers = mods;

        // Translate to ASCII for press events on non-modifier keys.
        ev.ascii = 0;
        if (!release && key < 128)
        {
            bool shifted = g_shiftHeld ^ g_capsLock;
            if (g_capsLock && !g_shiftHeld)
            {
                char c = g_scancodeToAsciiShift[key];
                ev.ascii = (c >= 'A' && c <= 'Z') ? c : g_scancodeToAscii[key];
            }
            else
            {
                ev.ascii = shifted ? g_scancodeToAsciiShift[key] : g_scancodeToAscii[key];
            }
        }
        InputDevicePush(&g_kbdInputDev, ev);
    }

    // Also push ASCII to the legacy ring buffer (for kernel shell / KbdGetChar).
    if (!release && key < 128)
    {
        bool shifted = g_shiftHeld ^ g_capsLock;
        char c;
        if (g_capsLock && !g_shiftHeld)
        {
            c = g_scancodeToAsciiShift[key];
            if (c < 'A' || c > 'Z') c = g_scancodeToAscii[key];
        }
        else
        {
            c = shifted ? g_scancodeToAsciiShift[key] : g_scancodeToAscii[key];
        }
        if (c) BufPush(c);
    }

    // Send EOI to LAPIC.
    ApicSendEoi();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Tracks whether KbdInit() has been called.
static bool g_kbdInitialized = false;

void KbdInit()
{
    // Flush any stale bytes in the PS/2 output buffer.
    while (inb(0x64) & 0x01) inb(0x60);

    // Register with the generic input subsystem.
    InputRegister(&g_kbdInputDev);

    // Install the IRQ handler and unmask IRQ1 in the I/O APIC.
    IdtInstallHandler(KBD_IRQ_VECTOR,
                      reinterpret_cast<void*>(KbdIrqHandler));
    IoApicUnmaskIrq(1, KBD_IRQ_VECTOR);

    g_kbdInitialized = true;
    KPuts("KBD: PS/2 keyboard ready (IRQ1 → vector 33)\n");
}

bool KbdIsAvailable()
{
    return g_kbdInitialized;
}

char KbdPeekChar()
{
    if (BufEmpty()) return 0;
    return BufPop();
}

char KbdGetChar()
{
    while (BufEmpty()) { __asm__ volatile("pause"); }
    return BufPop();
}

} // namespace brook

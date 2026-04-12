// mouse.cpp — compiled with -mgeneral-regs-only (ISR file).
//
// PS/2 mouse driver using the auxiliary port on the 8042 controller.
// Receives 3-byte packets: [status, deltaX, deltaY].
// Updates cursor position and pushes events to the input subsystem.

#include "mouse.h"
#include "input.h"
#include "idt.h"
#include "apic.h"
#include "serial.h"
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

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Wait for the 8042 input buffer to be empty (ready to accept a command).
static void WaitWrite()
{
    for (int i = 0; i < 100000; i++)
        if ((inb(0x64) & 0x02) == 0) return;
}

// Wait for the 8042 output buffer to have data.
static void WaitRead()
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01) return;
}

// Send a command byte to the PS/2 auxiliary (mouse) port.
static void MouseCmd(uint8_t cmd)
{
    WaitWrite();
    outb(0x64, 0xD4);   // Tell 8042: next byte goes to auxiliary device
    WaitWrite();
    outb(0x60, cmd);
    WaitRead();
    inb(0x60);           // Read and discard ACK (0xFA)
}

// Send a command to the 8042 controller itself.
static void CtrlCmd(uint8_t cmd)
{
    WaitWrite();
    outb(0x64, cmd);
}

// ---------------------------------------------------------------------------
// Mouse state
// ---------------------------------------------------------------------------

static volatile int32_t  g_mouseX   = 0;
static volatile int32_t  g_mouseY   = 0;
static volatile uint32_t g_maxX     = 1920;
static volatile uint32_t g_maxY     = 1080;
static volatile uint8_t  g_buttons  = 0;
static volatile uint8_t  g_prevButtons = 0;
static bool              g_mouseInit = false;

// 3-byte packet assembly
static uint8_t  g_packetBuf[3];
static uint8_t  g_packetIdx = 0;

// Input device for the input subsystem
static InputDeviceOps g_mouseOps = { "ps2_mouse", nullptr };
static InputDevice    g_mouseInputDev = { &g_mouseOps, {}, 0, 0, nullptr };

// ---------------------------------------------------------------------------
// IRQ12 interrupt handler
// ---------------------------------------------------------------------------

__attribute__((interrupt))
static void MouseIrqHandler(InterruptFrame* frame)
{
    (void)frame;

    // On IOAPIC-based systems, the status register may already be clear
    // by the time we read it. Since IRQ12 only fires for mouse data,
    // just read the data byte directly. If OBF isn't set, send EOI and
    // return (spurious interrupt).
    uint8_t status = inb(0x64);
    if (!(status & 0x01))
    {
        ApicSendEoi();
        return;
    }

    uint8_t data = inb(0x60);

    // Track IRQ count for diagnostics
    static volatile uint32_t s_irqCount = 0;
    uint32_t count = ++s_irqCount;
    if (count <= 3 || (count & 0xFF) == 0)
    {
        SerialPrintf("MOUSE IRQ: #%u status=0x%02x data=0x%02x idx=%u\n",
                     count, status, data, g_packetIdx);
    }

    // Synchronise to packet boundary: status byte (byte 0) always has bit 3 set.
    if (g_packetIdx == 0 && !(data & 0x08))
    {
        // Out of sync — discard and wait for a valid status byte
        ApicSendEoi();
        return;
    }

    g_packetBuf[g_packetIdx++] = data;

    if (g_packetIdx < 3)
    {
        ApicSendEoi();
        return;
    }

    // Full 3-byte packet received
    g_packetIdx = 0;

    uint8_t stat = g_packetBuf[0];
    int16_t dx   = static_cast<int16_t>(g_packetBuf[1]);
    int16_t dy   = static_cast<int16_t>(g_packetBuf[2]);

    // Sign-extend using overflow bits from status byte
    if (stat & 0x10) dx |= static_cast<int16_t>(0xFF00);
    if (stat & 0x20) dy |= static_cast<int16_t>(0xFF00);

    // Discard packets with overflow
    if (stat & 0xC0)
    {
        ApicSendEoi();
        return;
    }

    // Update cursor position (Y is inverted in PS/2 protocol)
    int32_t newX = g_mouseX + dx;
    int32_t newY = g_mouseY - dy;

    // Clamp to screen bounds
    if (newX < 0) newX = 0;
    if (newY < 0) newY = 0;
    if (newX >= static_cast<int32_t>(g_maxX)) newX = static_cast<int32_t>(g_maxX) - 1;
    if (newY >= static_cast<int32_t>(g_maxY)) newY = static_cast<int32_t>(g_maxY) - 1;

    g_mouseX = newX;
    g_mouseY = newY;

    // Update button state
    g_prevButtons = g_buttons;
    g_buttons = stat & 0x07;

    // Push move event to input subsystem
    if (dx != 0 || dy != 0)
    {
        InputEvent ev;
        ev.type     = InputEventType::MouseMove;
        ev.scanCode = g_buttons;
        ev.ascii    = 0;
        ev.modifiers = 0;
        // Pack deltaX/deltaY into scanCode + modifiers fields isn't ideal,
        // but for now the compositor reads g_mouseX/Y directly.
        InputDevicePush(&g_mouseInputDev, ev);
    }

    // Push button change events
    uint8_t changed = g_buttons ^ g_prevButtons;
    for (uint8_t bit = 0; bit < 3; bit++)
    {
        if (changed & (1 << bit))
        {
            InputEvent ev;
            ev.type     = (g_buttons & (1 << bit))
                            ? InputEventType::MouseButtonDown
                            : InputEventType::MouseButtonUp;
            ev.scanCode = bit; // 0=left, 1=right, 2=middle
            ev.ascii    = 0;
            ev.modifiers = g_buttons;
            InputDevicePush(&g_mouseInputDev, ev);
        }
    }

    ApicSendEoi();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MouseInit()
{
    if (g_mouseInit) return;

    SerialPuts("MOUSE: initialising PS/2 auxiliary device\n");

    // Flush any stale bytes
    while (inb(0x64) & 0x01) inb(0x60);

    // Enable the auxiliary (mouse) port on the 8042 controller first.
    CtrlCmd(0xA8);

    // Read current config byte, set bit 1 (enable IRQ12), clear bit 5 (enable aux clock).
    CtrlCmd(0x20);           // Read command byte
    WaitRead();
    uint8_t cfg = inb(0x60);
    SerialPrintf("MOUSE: 8042 config byte = 0x%02x\n", cfg);
    cfg |=  0x02;            // Enable IRQ12 (auxiliary interrupt)
    cfg &= ~0x20;            // Enable auxiliary clock
    CtrlCmd(0x60);           // Write command byte
    WaitWrite();
    outb(0x60, cfg);

    // Reset mouse
    MouseCmd(0xFF);
    // The reset sends back 0xFA (ACK) + 0xAA (self-test passed) + 0x00 (device ID)
    // We already consumed ACK in MouseCmd. Read the remaining bytes.
    WaitRead();
    uint8_t selfTest = inb(0x60);
    WaitRead();
    uint8_t devId = inb(0x60);
    SerialPrintf("MOUSE: reset response: self-test=0x%02x id=0x%02x\n", selfTest, devId);

    if (selfTest != 0xAA) {
        SerialPuts("MOUSE: self-test failed, aborting\n");
        return;
    }

    // Use default settings
    MouseCmd(0xF6);

    // Set sample rate to 100 samples/sec
    MouseCmd(0xF3);
    MouseCmd(100);  // Send rate as a mouse command (gets 0xD4 prefix + ACK)

    // Enable data reporting
    MouseCmd(0xF4);

    // Flush any residual data from init
    while (inb(0x64) & 0x01) inb(0x60);

    // Register with the input subsystem
    InputRegister(&g_mouseInputDev);

    // Install IRQ12 handler
    IdtInstallHandler(MOUSE_IRQ_VECTOR,
                      reinterpret_cast<void*>(MouseIrqHandler));
    IoApicUnmaskIrq(12, MOUSE_IRQ_VECTOR);

    g_mouseInit = true;
    KPuts("MOUSE: PS/2 mouse ready (IRQ12 → vector 44)\n");
}

bool MouseIsAvailable()
{
    return g_mouseInit;
}

void MouseGetPosition(int32_t* x, int32_t* y)
{
    if (x) *x = g_mouseX;
    if (y) *y = g_mouseY;
}

void MouseSetBounds(uint32_t maxX, uint32_t maxY)
{
    g_maxX = maxX;
    g_maxY = maxY;
    // Clamp current position
    if (g_mouseX >= static_cast<int32_t>(maxX)) g_mouseX = static_cast<int32_t>(maxX) - 1;
    if (g_mouseY >= static_cast<int32_t>(maxY)) g_mouseY = static_cast<int32_t>(maxY) - 1;
}

uint8_t MouseGetButtons()
{
    return g_buttons;
}

} // namespace brook

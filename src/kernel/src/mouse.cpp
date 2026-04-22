// mouse.cpp — compiled with -mgeneral-regs-only (ISR file).
//
// PS/2 mouse driver using the auxiliary port on the 8042 controller.
// Receives 3-byte packets: [status, deltaX, deltaY].
// Updates cursor position and pushes events to the input subsystem.

#include "mouse.h"
#include "input.h"
#include "compositor.h"
#include "idt.h"
#include "apic.h"
#include "serial.h"
#include "kprintf.h"
#include "portio.h"

namespace brook {

// ---------------------------------------------------------------------------
// Port I/O
// ---------------------------------------------------------------------------

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
static uint8_t           g_mouseId   = 0;   // 0=3-byte legacy, 3=IntelliMouse wheel, 4=Explorer (5 btn + H wheel)
static uint8_t           g_packetLen = 3;

// 4-byte packet assembly for IntelliMouse modes.
static uint8_t  g_packetBuf[4];
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

    // On IOAPIC-based systems, IRQ12 fires specifically for mouse data.
    // Check that output buffer is full (bit 0 OBF). We intentionally do NOT
    // require the AUXB bit (bit 5) because some QEMU/KVM versions don't set
    // it reliably. Since IRQ12 only fires for mouse, OBF alone is sufficient.
    uint8_t status = inb(0x64);
    if (!(status & 0x01))
    {
        ApicSendEoi();
        return;
    }

    uint8_t data = inb(0x60);

    // Track IRQ count — lockless UART output to avoid serial-lock deadlock
    // in ISR context (the serial lock may be held with sti during spin-wait).
    static volatile uint32_t s_irqCount = 0;
    uint32_t count = ++s_irqCount;
    if (count <= 3 || (count & 0xFF) == 0)
    {
        const char* msg = "MOUSE IRQ\r\n";
        while (*msg) {
            while ((inb(0x3FD) & 0x20) == 0) {}
            outb(0x3F8, static_cast<uint8_t>(*msg++));
        }
    }

    // Synchronise to packet boundary: status byte (byte 0) always has bit 3 set.
    if (g_packetIdx == 0 && !(data & 0x08))
    {
        // Out of sync — discard and wait for a valid status byte
        ApicSendEoi();
        return;
    }

    g_packetBuf[g_packetIdx++] = data;

    if (g_packetIdx < g_packetLen)
    {
        ApicSendEoi();
        return;
    }

    // Full packet received
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

    // Decode wheel bytes for IntelliMouse / Explorer modes.
    int8_t wheelY = 0;  // vertical scroll ticks (up positive)
    int8_t wheelX = 0;  // horizontal scroll ticks (right positive)
    uint8_t extraButtons = 0; // bits 0=btn4, 1=btn5
    if (g_packetLen == 4)
    {
        uint8_t z = g_packetBuf[3];
        if (g_mouseId == 3)
        {
            // Plain IntelliMouse: byte 3 is a signed int8 vertical Z delta.
            // IntelliMouse convention: positive = scroll DOWN.  Flip so our
            // event type uses "up positive".
            wheelY = static_cast<int8_t>(-static_cast<int8_t>(z));
        }
        else if (g_mouseId == 4)
        {
            // IntelliMouse Explorer: low nibble is signed 4-bit Z (vertical),
            // bits 6/7 carry buttons 4/5.  Some firmwares repurpose bits 4/5
            // for horizontal scroll; handle it when present.
            int8_t zlo = static_cast<int8_t>(z & 0x0F);
            if (zlo & 0x08) zlo |= static_cast<int8_t>(0xF0); // sign-extend
            // Flip: PS/2 convention is +ve = down, we want up positive.
            wheelY = static_cast<int8_t>(-zlo);
            extraButtons = (z >> 6) & 0x03;
            // Horizontal scroll: some mice set bit 5 for right, bit 4 for left.
            if (z & 0x20) wheelX = 1;
            if (z & 0x10) wheelX = -1;
        }
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

    // Update button state (core 3 in stat, extras from wheel byte)
    g_prevButtons = g_buttons;
    g_buttons = static_cast<uint8_t>((stat & 0x07) | ((extraButtons & 0x03) << 3));

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

    // Push scroll event
    if (wheelY != 0 || wheelX != 0)
    {
        InputEvent ev;
        ev.type     = InputEventType::MouseScroll;
        ev.scanCode = static_cast<uint8_t>(wheelY); // int8 dy (up positive)
        ev.ascii    = static_cast<char>(wheelX);    // int8 dx (right positive)
        ev.modifiers = g_buttons;
        InputDevicePush(&g_mouseInputDev, ev);
    }

    // Push button change events
    uint8_t changed = g_buttons ^ g_prevButtons;
    for (uint8_t bit = 0; bit < 5; bit++)
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

    // Wake the compositor so the cursor redraws immediately.
    CompositorWake();

    // Wake any processes blocked on input (e.g. for future /dev/mouse reads).
    InputWakeWaiters();

    ApicSendEoi();
}
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
    SerialPrintf("MOUSE: 8042 config byte = 0x%x\n", cfg);
    cfg |=  0x02;            // Enable IRQ12 (auxiliary interrupt)
    cfg &= ~0x20;            // Enable auxiliary clock
    CtrlCmd(0x60);           // Write command byte
    WaitWrite();
    outb(0x60, cfg);

    // Reset mouse — sends 0xFF via auxiliary port.
    // Response: 0xFA (ACK) + 0xAA (self-test) + 0x00 (device ID)
    // MouseCmd already consumes the ACK; read the remaining two bytes.
    MouseCmd(0xFF);

    // The self-test response can take up to 500ms. Retry reads.
    uint8_t selfTest = 0;
    for (int retry = 0; retry < 10; retry++)
    {
        WaitRead();
        uint8_t b = inb(0x60);
        if (b == 0xAA) { selfTest = b; break; }
        if (b == 0xFC) { selfTest = b; break; } // self-test failed
        // 0xFA is a late ACK — skip it
    }

    uint8_t devId = 0;
    if (selfTest == 0xAA)
    {
        WaitRead();
        devId = inb(0x60);
    }

    SerialPrintf("MOUSE: reset response: self-test=0x%x id=0x%x\n", selfTest, devId);

    if (selfTest != 0xAA) {
        SerialPuts("MOUSE: self-test failed, aborting\n");
        return;
    }

    // Use default settings
    MouseCmd(0xF6);

    // IntelliMouse Explorer knock sequence.  The magic sample-rate
    // sequence enables extended PS/2 protocols:
    //   200, 100, 80  → device ID 3 (IntelliMouse: +1 byte vertical wheel)
    //   200, 200, 80  → device ID 4 (Explorer: +1 byte wheel + 2 buttons,
    //                                 some firmwares encode horizontal
    //                                 scroll in upper bits of the Z byte)
    // We only move to the next knock if the previous one succeeded.
    auto tryKnock = [](const uint8_t* rates) {
        for (int i = 0; i < 3; i++) {
            MouseCmd(0xF3);   // Set sample rate
            MouseCmd(rates[i]);
        }
    };
    auto readId = []() -> uint8_t {
        MouseCmd(0xF2);       // Get device ID (ACK consumed)
        WaitRead();
        return inb(0x60);
    };

    static const uint8_t KNOCK_WHEEL[3]   = { 200, 100, 80 };
    static const uint8_t KNOCK_EXPLORER[3] = { 200, 200, 80 };

    tryKnock(KNOCK_WHEEL);
    uint8_t id = readId();
    if (id == 3)
    {
        g_mouseId = 3;
        g_packetLen = 4;
        tryKnock(KNOCK_EXPLORER);
        uint8_t id2 = readId();
        if (id2 == 4)
        {
            g_mouseId = 4;
        }
    }
    SerialPrintf("MOUSE: negotiated protocol id=%u (%u-byte packets)\n",
                 g_mouseId, g_packetLen);

    // Set sample rate to 100 samples/sec
    MouseCmd(0xF3);
    MouseCmd(100);

    // Set resolution to 4 counts/mm (highest)
    MouseCmd(0xE8);
    MouseCmd(0x03);

    // Enable data reporting — this is critical: without it, the mouse
    // generates no data (and thus no IRQ12).
    MouseCmd(0xF4);

    // Re-read config to confirm IRQ12 is enabled
    CtrlCmd(0x20);
    WaitRead();
    uint8_t cfgAfter = inb(0x60);
    SerialPrintf("MOUSE: 8042 config after init = 0x%x\n", cfgAfter);

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

void MouseSetPosition(int32_t x, int32_t y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= static_cast<int32_t>(g_maxX)) x = static_cast<int32_t>(g_maxX) - 1;
    if (y >= static_cast<int32_t>(g_maxY)) y = static_cast<int32_t>(g_maxY) - 1;
    g_mouseX = x;
    g_mouseY = y;
}

void MouseSetButtons(uint8_t buttons)
{
    g_buttons = buttons;
}

void MouseSetAvailable(bool available)
{
    g_mouseInit = available;
}

} // namespace brook

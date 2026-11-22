// mouse.cpp — compiled with -mgeneral-regs-only (ISR file).
//
// PS/2 mouse driver using the auxiliary port on the 8042 controller.
// Receives 3-byte packets: [status, deltaX, deltaY].
// Updates cursor position and pushes events to the input subsystem.

#include "mouse.h"
#include "spinlock.h"
#include "desktop_pointer_motion.h"
#include "vmware_platform.h"
#include "input.h"
#include "compositor.h"
#include "idt.h"
#include "apic.h"
#include "serial.h"
#include "kprintf.h"
#include "portio.h"
#include "irq_wrapper.h"
#include "keyboard.h"
#include "synaptics_touchpad.h"
#include "synaptics_tap.h"

namespace brook {
uint32_t WmGetAppliedScale();

// ---------------------------------------------------------------------------
// Port I/O
// ---------------------------------------------------------------------------

// Wait for the 8042 input buffer to be empty (ready to accept a command).
static void WaitWrite()
{
    const uint64_t start = ApicTickCount();
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) return;
        if ((i & 63) == 63 && ApicTickCount() - start >= 50) return;
    }
}

// Wait for the 8042 output buffer to have data. Bounded by both an
// iteration count (the historic bound, valid even without a running clock)
// and elapsed time: where firmware emulates the 8042 in SMM (USB legacy
// support on desktop boards), one status read can take several microseconds,
// so 100000 reads were ~0.5 s per wait with no mouse attached.
static bool WaitReadMs(uint64_t ms)
{
    const uint64_t start = ApicTickCount();
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) return true;
        if ((i & 63) == 63 && ApicTickCount() - start >= ms) return false;
    }
    return false;
}
static void WaitRead() { (void)WaitReadMs(100); }

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


// Checked variants used for Synaptics negotiation: a missing ACK or reply is
// reported instead of silently consuming whatever byte comes next.
static bool AuxRead(uint8_t& out)
{
    for (int i = 0; i < 200000; i++)
        if (inb(0x64) & 0x01) { out = inb(0x60); return true; }
    return false;
}
static bool AuxCommand(uint8_t cmd)
{
    WaitWrite();
    outb(0x64, 0xD4);
    WaitWrite();
    outb(0x60, cmd);
    uint8_t ack = 0;
    return AuxRead(ack) && ack == 0xFA;
}
// Synaptics "special" command: E6 then four E8 carrying the argument 2 bits at a time.
static bool SynSpecial(uint8_t arg)
{
    uint8_t slice[4];
    SynapticsSlice(arg, slice);
    if (!AuxCommand(0xE6)) return false;
    for (int i = 0; i < 4; i++)
        if (!AuxCommand(0xE8) || !AuxCommand(slice[i])) return false;
    return true;
}
static bool SynQuery(uint8_t query, uint8_t r[3])
{
    if (!SynSpecial(query) || !AuxCommand(0xE9)) return false;
    for (int i = 0; i < 3; i++) if (!AuxRead(r[i])) return false;
    return true;
}
static bool SynSetMode(uint8_t mode)
{
    return SynSpecial(mode) && AuxCommand(0xF3) && AuxCommand(0x14);
}

// ---------------------------------------------------------------------------
// Mouse state
// ---------------------------------------------------------------------------

static IrqSpinLock g_pointerPositionLock;
static int32_t g_fractionX=0,g_fractionY=0;
static uint32_t g_motionScale=100;
static volatile int32_t  g_mouseX   = 0;
static volatile int32_t  g_mouseY   = 0;
static volatile uint32_t g_maxX     = 1920;
static volatile uint32_t g_maxY     = 1080;
static volatile uint8_t  g_buttons  = 0;
static volatile uint8_t  g_prevButtons = 0;
static bool              g_mouseInit = false;
static uint8_t           g_mouseId   = 0;   // 0=3-byte legacy, 3=IntelliMouse wheel, 4=Explorer (5 btn + H wheel)
static uint8_t           g_packetLen = 3;

// Synaptics absolute mode (6-byte packets). g_synWanted is the runtime switch
// (boot default on); g_synActive is set only after detection and mode set
// succeeded, otherwise the relative IntelliMouse path below stays in use.
static bool                 g_synWanted = true;
static volatile bool        g_synActive = false;
static SynapticsIdentity    g_synId;
static SynapticsCaps        g_synCaps;
static uint8_t              g_synMode = 0;
static uint8_t              g_synModeReadback = 0;
static bool                 g_synAgm = false;
// Raw 3-byte replies: identify, capabilities, ext 0x09, ext 0x0c, modes, max
// coords 0x0d, min coords 0x0f; bit i of g_synRawValid set when reply i was read.
static uint8_t              g_synRaw[MouseTouchpadRawReplies][3];
static SynapticsRange       g_synRange;
static uint8_t              g_synRawValid = 0;
static const char*          g_synReason = "not attempted";
static SynapticsGesture     g_synGesture;
static volatile uint32_t    g_synPackets = 0, g_synResyncs = 0, g_synScrollTicks = 0;
// Tap-to-click (user setting, default off). g_synLock serialises the gesture
// and tap state between IRQ12, the LAPIC timer poll that releases a tap whose
// drag window expired without further packets, and the runtime setters.
static IrqSpinLock          g_synLock;
static SynapticsTap         g_synTap;
static uint8_t              g_synPhysButtons = 0;   // physical buttons as last reported
volatile uint64_t           g_touchpadTapDeadline = 0;
// Raw packet trace (debug bridge "input touchpad trace"): a ring of the most
// recent packets, written under g_synLock only while g_synTraceOn.
static volatile bool        g_synTraceOn = false;
static MouseTouchpadTraceEntry g_synTrace[MouseTouchpadTraceCapacity];
static uint32_t             g_synTraceTotal = 0;
static uint64_t             g_synTraceStartMs = 0;

// 4-byte packet assembly for IntelliMouse modes.
static uint8_t  g_packetBuf[6];
static uint8_t  g_packetIdx = 0;

// Input device for the input subsystem
static InputDeviceOps g_mouseOps = { "ps2_mouse", nullptr };
static InputDevice    g_mouseInputDev = { &g_mouseOps, {}, 0, 0, nullptr };

// Push motion, scroll and button events for one decoded packet.
static void EmitPointer(int32_t dx, int32_t dyDown, int8_t wheelY, int8_t wheelX, uint8_t buttons,
                        int32_t fineY120 = 0, int32_t fineX120 = 0, uint8_t scrollFlags = 0)
{
    // A wheel tick is 120; a touchpad reports its finer motion as well.
    if (!fineY120) fineY120 = int32_t(wheelY) * 120;
    if (!fineX120) fineX120 = int32_t(wheelX) * 120;
    // Only the desktop position is scaled; the event below retains raw deltas.
    MouseMoveRelative(dx, dyDown);
    g_prevButtons = g_buttons;
    g_buttons = buttons;
    if (dx != 0 || dyDown != 0)
    {
        InputEvent ev{};
        ev.type     = InputEventType::MouseRelativeMove;
        ev.scanCode = g_buttons;
        ev.deltaX = dx;
        ev.deltaY = dyDown;
        InputDevicePush(&g_mouseInputDev, ev);
    }
    if (wheelY || wheelX || fineY120 || fineX120 || (scrollFlags & BROOK_WM_SCROLL_END))
    {
        InputEvent ev{};
        ev.type     = InputEventType::MouseScroll;
        ev.scanCode = static_cast<uint8_t>(wheelY); // int8 dy (up positive)
        ev.ascii    = static_cast<char>(wheelX);    // int8 dx (right positive)
        ev.modifiers = g_buttons;
        ev.scrollFlags = scrollFlags;
        ev.deltaY = fineY120;
        ev.deltaX = fineX120;
        InputDevicePush(&g_mouseInputDev, ev);
    }
    uint8_t changed = g_buttons ^ g_prevButtons;
    for (uint8_t bit = 0; bit < 5; bit++)
    {
        if (changed & (1 << bit))
        {
            InputEvent ev{};
            ev.type     = (g_buttons & (1 << bit)) ? InputEventType::MouseButtonDown
                                                   : InputEventType::MouseButtonUp;
            ev.scanCode = bit;
            ev.modifiers = g_buttons;
            InputDevicePush(&g_mouseInputDev, ev);
        }
    }
    CompositorWake();
    InputWakeWaiters();
}

static int8_t ClampTicks(int32_t v) { return static_cast<int8_t>(v > 127 ? 127 : v < -127 ? -127 : v); }

// Tap button transitions, each reported as its own state change on top of the
// physical buttons (g_synLock held).
static void EmitTapSteps(const SynapticsTapOutput& t)
{
    for (uint8_t i = 0; i < t.steps; ++i)
        EmitPointer(0, 0, 0, 0, static_cast<uint8_t>(g_synPhysButtons | t.mask[i]));
}
static void PublishTapDeadline()
{
    __atomic_store_n(&g_touchpadTapDeadline, g_synTap.Deadline(), __ATOMIC_RELEASE);
}

// Synaptics absolute-mode byte handler (IRQ context).
static void SynapticsByte(uint8_t data)
{
    if (!SynapticsByteValid(g_packetIdx, data))
    {
        ++g_synResyncs;
        // A rejected byte may be the start of the next packet. Contact state
        // from before the loss is no longer trustworthy.
        const auto flags = IrqSpinLockAcquire(&g_synLock);
        if (g_synGesture.Reset())
            EmitPointer(0, 0, 0, 0, g_buttons, 0, 0, BROOK_WM_SCROLL_FINGER | BROOK_WM_SCROLL_END);
        EmitTapSteps(g_synTap.Reset());
        g_synGesture.SetTapDrag(false);
        PublishTapDeadline();
        IrqSpinLockRelease(&g_synLock, flags);
        g_packetIdx = 0;
        if (!SynapticsByteValid(0, data)) return;
    }
    g_packetBuf[g_packetIdx++] = data;
    if (g_packetIdx < 6) return;
    g_packetIdx = 0;
    ++g_synPackets;
    const SynapticsPacket p = SynapticsDecodePacket(g_packetBuf, g_synCaps.clickpad());
    const auto flags = IrqSpinLockAcquire(&g_synLock);
    const SynapticsOutput o = g_synGesture.Feed(p);
    if (g_synTraceOn) {
        auto& e = g_synTrace[g_synTraceTotal % MouseTouchpadTraceCapacity];
        e.ms = static_cast<uint32_t>(ApicTickCount() - g_synTraceStartMs);
        for (int i = 0; i < 6; ++i) e.bytes[i] = g_packetBuf[i];
        e.buttons = o.buttons;
        ++g_synTraceTotal;
    }
    if (p.w != 2) {   // extended (second-finger) packets carry no contact summary
        EmitTapSteps(g_synTap.Feed(SynapticsTapInputFrom(p, g_synGesture, ApicTickCount())));
        g_synGesture.SetTapDrag(g_synTap.Dragging());
        PublishTapDeadline();
    }
    g_synPhysButtons = o.buttons;
    const uint8_t buttons = static_cast<uint8_t>(o.buttons | g_synTap.Held());
    if (o.scrollY || o.scrollX) g_synScrollTicks += uint32_t((o.scrollY < 0 ? -o.scrollY : o.scrollY) + (o.scrollX < 0 ? -o.scrollX : o.scrollX));
    if (o.dx || o.dy || o.scrollY || o.scrollX || o.scrollY120 || o.scrollX120 || o.scrollEnded || buttons != g_buttons)
        EmitPointer(o.dx, o.dy, ClampTicks(o.scrollY), ClampTicks(o.scrollX), buttons,
                    o.scrollY120, o.scrollX120,
                    BROOK_WM_SCROLL_FINGER | (o.scrollEnded ? BROOK_WM_SCROLL_END : 0));
    IrqSpinLockRelease(&g_synLock, flags);
}

// LAPIC timer (BSP) once g_touchpadTapDeadline has passed: release a tap whose
// drag window closed while the pad sent no packets.
void MouseTouchpadTapPoll(uint64_t nowMs)
{
    const auto flags = IrqSpinLockAcquire(&g_synLock);
    EmitTapSteps(g_synTap.Timeout(nowMs));
    PublishTapDeadline();
    IrqSpinLockRelease(&g_synLock, flags);
}

// ---------------------------------------------------------------------------
// IRQ12 interrupt handler
// ---------------------------------------------------------------------------

extern "C" void MouseIrqHandlerInner(void)
{

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

    if (g_synActive)
    {
        SynapticsByte(data);
        ApicSendEoi();
        return;
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

    // Only the desktop position is scaled; the event below retains raw deltas.
    MouseMoveRelative(dx, -dy);

    // Update button state (core 3 in stat, extras from wheel byte)
    g_prevButtons = g_buttons;
    g_buttons = static_cast<uint8_t>((stat & 0x07) | ((extraButtons & 0x03) << 3));

    // Push move event to input subsystem
    if (dx != 0 || dy != 0)
    {
        InputEvent ev{};
        ev.type     = InputEventType::MouseRelativeMove;
        ev.scanCode = g_buttons;
        ev.ascii    = 0;
        ev.modifiers = 0;
        ev.deltaX = dx;
        ev.deltaY = -dy;
        InputDevicePush(&g_mouseInputDev, ev);
    }

    // Push scroll event
    if (wheelY != 0 || wheelX != 0)
    {
        InputEvent ev{};
        ev.type     = InputEventType::MouseScroll;
        ev.scanCode = static_cast<uint8_t>(wheelY); // int8 dy (up positive)
        ev.ascii    = static_cast<char>(wheelX);    // int8 dx (right positive)
        ev.modifiers = g_buttons;
        ev.deltaY = int32_t(wheelY) * 120;
        ev.deltaX = int32_t(wheelX) * 120;
        InputDevicePush(&g_mouseInputDev, ev);
    }

    // Push button change events
    uint8_t changed = g_buttons ^ g_prevButtons;
    if (changed)
    {
        // First-N trace per button-state change so we can debug missing
        // middle-button events on real hardware.  Capped to keep serial quiet.
        static uint32_t s_btnTraces = 0;
        if (s_btnTraces++ < 32)
        {
            SerialPrintf("MOUSE: stat=0x%02x extras=0x%02x buttons=0x%02x "
                         "prev=0x%02x changed=0x%02x packetLen=%u id=%u\r\n",
                         stat, extraButtons, g_buttons, g_prevButtons,
                         changed, g_packetLen, g_mouseId);
        }
    }
    for (uint8_t bit = 0; bit < 5; bit++)
    {
        if (changed & (1 << bit))
        {
            InputEvent ev{};
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

// Naked ISR wrapper — handles SWAPGS + GPR save/restore around the inner handler.
IRQ_NAKED_HANDLER(MouseIrqHandler, MouseIrqHandlerInner)

// ---------------------------------------------------------------------------

// Relative PS/2: IntelliMouse / Explorer negotiation, 100 Hz, 4 counts/mm.
static void NegotiateRelative()
{
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
    g_mouseId = 0;
    g_packetLen = 3;
    tryKnock(KNOCK_WHEEL);
    uint8_t id = readId();
    if (id == 3)
    {
        g_mouseId = 3;
        g_packetLen = 4;
        tryKnock(KNOCK_EXPLORER);
        uint8_t id2 = readId();
        if (id2 == 4) g_mouseId = 4;
    }
    SerialPrintf("MOUSE: negotiated protocol id=%u (%u-byte packets)\n", g_mouseId, g_packetLen);
    MouseCmd(0xF3);   // 100 samples/sec
    MouseCmd(100);
    MouseCmd(0xE8);   // 4 counts/mm
    MouseCmd(0x03);
}

static bool SynRawQuery(unsigned slot, uint8_t query, uint8_t r[3])
{
    if (!SynQuery(query, r)) return false;
    for (int i = 0; i < 3; ++i) g_synRaw[slot][i] = r[i];
    g_synRawValid |= uint8_t(1u << slot);
    return true;
}

// Identify and capability queries only (no mode change).
static bool SynapticsQueryAll(SynapticsCaps& caps)
{
    uint8_t r[3];
    g_synRawValid = 0;
    if (!SynRawQuery(0, SynQueryIdentify, r)) { g_synReason = "identify query failed"; return false; }
    g_synId = SynapticsDecodeIdentify(r);
    if (!g_synId.valid) { g_synReason = "not a Synaptics pad"; return false; }
    if (!SynRawQuery(1, SynQueryCapabilities, r)) { g_synReason = "capabilities query failed"; return false; }
    caps = SynapticsDecodeCaps(r, g_synId);
    g_synCaps = caps;
    if (!caps.valid) { g_synReason = "capabilities invalid"; return false; }
    if (caps.extRequests() >= 1 && SynRawQuery(2, SynQueryExtCap, r)) caps.ext = SynapticsWord(r);
    if (caps.extRequests() >= 4 && SynRawQuery(3, SynQueryExtCap0C, r)) caps.ext0c = SynapticsWord(r);
    g_synCaps = caps;
    // Coordinate range for the clickpad button areas (Linux synaptics_resolution).
    uint8_t mx[3], mn[3];
    const bool haveMax = caps.maxCoords() && SynRawQuery(5, SynQueryMaxCoords, mx);
    const bool haveMin = caps.minCoords() && SynRawQuery(6, SynQueryMinCoords, mn);
    g_synRange = SynapticsDecodeRange(haveMax ? mx : nullptr, haveMin ? mn : nullptr);
    return true;
}

static bool SynapticsTryEnable()
{
    uint8_t r[3];
    g_synModeReadback = 0;
    SynapticsCaps caps;
    if (!SynapticsQueryAll(caps)) return false;
    const uint8_t mode = SynapticsModeByte(caps);
    if (!SynSetMode(mode)) { g_synReason = "mode set failed"; (void)SynSetMode(0); return false; }
    if (!SynRawQuery(4, SynQueryModes, r)) { g_synReason = "modes query failed"; (void)SynSetMode(0); return false; }
    g_synModeReadback = r[2];
    if (!(r[2] & SynModeAbsolute)) { g_synReason = "absolute mode not confirmed"; (void)SynSetMode(0); return false; }
    g_synMode = mode;
    {
        const auto lockFlags = IrqSpinLockAcquire(&g_synLock);
        auto cfg = g_synGesture.Config();
        SynapticsSetButtonAreas(cfg, g_synRange, caps.clickpad());
        g_synGesture.SetConfig(cfg);
        IrqSpinLockRelease(&g_synLock, lockFlags);
    }
    // Advanced gesture mode (image-sensor pads): W=2 packets then report the
    // second finger, used for click-drag with the moving finger. Linux
    // synaptics_set_advanced_gesture_mode: sliced 0x03, then F3 0xC8.
    g_synAgm = caps.agm() && SynSpecial(0x03) && AuxCommand(0xF3) && AuxCommand(0xC8);
    g_synReason = caps.extended() ? "active" : "active (no W: two-finger scroll unavailable)";
    SerialPrintf("MOUSE: Synaptics %u.%u model %u caps=0x%06x ext=0x%06x ext0c=0x%06x clickpad=%u agm=%u mode=0x%02x\n",
                 g_synId.major, g_synId.minor, g_synId.model, caps.caps, caps.ext, caps.ext0c,
                 caps.clickpad() ? 1u : 0u, g_synAgm ? 1u : 0u, mode);
    SerialPrintf("MOUSE: Synaptics range x %d..%d y %d..%d (%s) button strip y<%d split x=%d\n",
                 g_synRange.xMin, g_synRange.xMax, g_synRange.yMin, g_synRange.yMax,
                 g_synRange.reportedMax ? "reported" : "nominal",
                 g_synGesture.Config().buttonTopY, g_synGesture.Config().buttonSplitX);
    return true;
}

static void ConfigureSynapticsPackets()
{
    g_mouseId = 0;
    g_packetLen = 6;
    g_packetIdx = 0;
    g_synActive = true;
}

void MouseInit()
{
    if (g_mouseInit) return;
    struct InterruptGuard {
        uint64_t flags;
        InterruptGuard() { __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory"); }
        ~InterruptGuard() { if(flags & (1ull<<9)) __asm__ volatile("sti":::"memory"); }
    } guard; // command-byte responses must not be consumed by IRQ1


    // VMware firmware/tools may leave the paravirtual pointer enabled. This
    // driver consumes ordinary relative PS/2 packets, not the backdoor queue.
    // Disable that queue before resetting the PS/2 mouse (Linux vmmouse reset
    // performs the same handoff). Do not issue these port commands elsewhere.
    if (VmwarePlatformPresent([](uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(0));
    })) {
        uint32_t a=0x564d5868u,b=0,c=40,d=0x5658;
        VmwareBackdoor(a,b,c,d);
        const auto before=a;
        if ((before&0xffff0000u)!=0xffff0000u) {
            a=0x564d5868u;b=0xf5;c=41;d=0x5658;
            VmwareBackdoor(a,b,c,d);
            a=0x564d5868u;b=0;c=40;d=0x5658;
            VmwareBackdoor(a,b,c,d);
        }
        SerialPrintf("MOUSE: VMware pointer queue before=0x%x after=0x%x (relative PS/2 driver)\n",before,a);
    }

    SerialPuts("MOUSE: initialising PS/2 auxiliary device\n");
    if (inb(0x64) == 0xff) {
        KPuts("MOUSE: no PS/2 controller (status 0xff); mouse unavailable\n");
        return;
    }

    // Flush any stale bytes
    for (unsigned i=0;i<32 && (inb(0x64)&1);++i) (void)inb(0x60);

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
    // A device ACKs within 25 ms (PS/2); with no byte at all after 200 ms
    // there is no auxiliary device, so skip the self-test wait.
    WaitWrite();
    outb(0x64, 0xD4);
    WaitWrite();
    outb(0x60, 0xFF);
    if (!WaitReadMs(200)) {
        SerialPuts("MOUSE: no reply to reset, aborting\n");
        KPrintf("MOUSE: no PS/2 auxiliary device (no reset reply); mouse unavailable\n");
        return;
    }
    (void)inb(0x60);     // ACK (0xFA)

    // The self-test response can take up to 500ms. Retry reads, bounded
    // to one second in total.
    uint8_t selfTest = 0;
    const uint64_t selfTestStart = ApicTickCount();
    for (int retry = 0; retry < 10; retry++)
    {
        const uint64_t spent = ApicTickCount() - selfTestStart;
        if (spent >= 1000 || !WaitReadMs(1000 - spent)) break;
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
        KPrintf("MOUSE: self-test failed (response=0x%x); mouse unavailable\n", selfTest);
        return;
    }

    // Use default settings
    MouseCmd(0xF6);

    // Synaptics absolute mode (two-finger scroll) when the pad identifies as
    // Synaptics; anything else, or any failed step, keeps the relative path.
    g_synActive = false;
    if (g_synWanted && SynapticsTryEnable()) ConfigureSynapticsPackets();
    else NegotiateRelative();

    // Enable data reporting — this is critical: without it, the mouse
    // generates no data (and thus no IRQ12).
    MouseCmd(0xF4);

    // Re-read config to confirm IRQ12 is enabled
    CtrlCmd(0x20);
    WaitRead();
    uint8_t cfgAfter = inb(0x60);
    SerialPrintf("MOUSE: 8042 config after init = 0x%x\n", cfgAfter);

    // Flush any residual data from init
    for (unsigned i=0;i<32 && (inb(0x64)&1);++i) (void)inb(0x60);

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
    const auto flags=IrqSpinLockAcquire(&g_pointerPositionLock);
    if (x) *x = g_mouseX;
    if (y) *y = g_mouseY;
    IrqSpinLockRelease(&g_pointerPositionLock,flags);
}

void MouseSetBounds(uint32_t maxX, uint32_t maxY)
{
    const auto flags=IrqSpinLockAcquire(&g_pointerPositionLock);
    g_maxX=maxX?maxX:1;g_maxY=maxY?maxY:1;
    if(g_mouseX>=static_cast<int32_t>(g_maxX))g_mouseX=static_cast<int32_t>(g_maxX)-1;
    if(g_mouseY>=static_cast<int32_t>(g_maxY))g_mouseY=static_cast<int32_t>(g_maxY)-1;
    g_fractionX=g_fractionY=0;
    IrqSpinLockRelease(&g_pointerPositionLock,flags);
}

void MouseMoveRelative(int32_t dx,int32_t dy)
{
    const uint32_t percent=WmGetAppliedScale();
    const auto flags=IrqSpinLockAcquire(&g_pointerPositionLock);
    if(g_motionScale!=percent){g_fractionX=g_fractionY=0;g_motionScale=percent;}
    g_mouseX=AdvanceDesktopPointer(g_mouseX,dx,g_maxX,percent,g_fractionX);
    g_mouseY=AdvanceDesktopPointer(g_mouseY,dy,g_maxY,percent,g_fractionY);
    IrqSpinLockRelease(&g_pointerPositionLock,flags);
}

uint8_t MouseGetButtons()
{
    return g_buttons;
}

void MouseSetPosition(int32_t x, int32_t y)
{
    const auto flags=IrqSpinLockAcquire(&g_pointerPositionLock);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= static_cast<int32_t>(g_maxX)) x = static_cast<int32_t>(g_maxX) - 1;
    if (y >= static_cast<int32_t>(g_maxY)) y = static_cast<int32_t>(g_maxY) - 1;
    g_mouseX = x;g_mouseY = y;
    g_fractionX=g_fractionY=0;
    IrqSpinLockRelease(&g_pointerPositionLock,flags);
}

void MouseSetButtons(uint8_t buttons)
{
    g_buttons = buttons;
}

void MouseSetAvailable(bool available)
{
    g_mouseInit = available;
}

// Runtime switch: renegotiate the PS/2 device in absolute (Synaptics) or
// relative mode. IRQ1 and IRQ12 are masked and local interrupts disabled so no
// handler consumes command replies from port 0x60.
bool MouseTouchpadSet(bool synaptics)
{
    if (!g_mouseInit) return false;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");
    IoApicMaskIrq(12);
    IoApicMaskIrq(1);
    (void)AuxCommand(0xF5);                      // stop reporting
    for (unsigned i = 0; i < 64 && (inb(0x64) & 1); ++i) (void)inb(0x60);
    g_synWanted = synaptics;
    const bool wasActive = g_synActive;
    g_synActive = false;
    g_packetIdx = 0;
    bool ok = true;
    if (synaptics && SynapticsTryEnable()) ConfigureSynapticsPackets();
    else {
        if (synaptics) ok = false;
        if (wasActive) (void)SynSetMode(0);       // back to relative PS/2
        if (!synaptics) g_synReason = "disabled";
        MouseCmd(0xF6);
        NegotiateRelative();
    }
    {
        const auto lockFlags = IrqSpinLockAcquire(&g_synLock);
        g_synGesture = SynapticsGesture(g_synGesture.Config());
        EmitTapSteps(g_synTap.Reset());
        g_synPhysButtons = 0;
        PublishTapDeadline();
        IrqSpinLockRelease(&g_synLock, lockFlags);
    }
    MouseCmd(0xF4);                              // resume reporting
    for (unsigned i = 0; i < 64 && (inb(0x64) & 1); ++i) (void)inb(0x60);
    g_packetIdx = 0;
    IoApicUnmaskIrq(1, KBD_IRQ_VECTOR);
    IoApicUnmaskIrq(12, MOUSE_IRQ_VECTOR);
    if (flags & (1ull << 9)) __asm__ volatile("sti":::"memory");
    return ok;
}

// Read-only diagnostic: pause reporting, rerun the identify/capability/modes
// queries and record the raw replies, then resume in the current mode.
bool MouseTouchpadProbe()
{
    if (!g_mouseInit) return false;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");
    IoApicMaskIrq(12);
    IoApicMaskIrq(1);
    (void)AuxCommand(0xF5);
    for (unsigned i = 0; i < 64 && (inb(0x64) & 1); ++i) (void)inb(0x60);
    const char* reason = g_synReason;
    SynapticsCaps caps;
    bool ok = SynapticsQueryAll(caps);
    uint8_t r[3];
    if (ok) ok = SynRawQuery(4, SynQueryModes, r);
    if (g_synActive) g_synReason = reason;      // keep the live state's reason
    (void)AuxCommand(0xF4);
    for (unsigned i = 0; i < 64 && (inb(0x64) & 1); ++i) (void)inb(0x60);
    g_packetIdx = 0;
    IoApicUnmaskIrq(1, KBD_IRQ_VECTOR);
    IoApicUnmaskIrq(12, MOUSE_IRQ_VECTOR);
    if (flags & (1ull << 9)) __asm__ volatile("sti":::"memory");
    return ok;
}

bool MouseTouchpadSetSpeed(uint32_t percent)
{
    if (percent < uint32_t(SynapticsSpeedMin) || percent > uint32_t(SynapticsSpeedMax)) return false;
    const auto flags = IrqSpinLockAcquire(&g_synLock);
    auto cfg = g_synGesture.Config();
    cfg.speedPercent = int32_t(percent);
    g_synGesture.SetConfig(cfg);
    IrqSpinLockRelease(&g_synLock, flags);
    return true;
}
uint32_t MouseTouchpadSpeed() { return uint32_t(g_synGesture.Config().speedPercent); }

void MouseTouchpadSetTap(bool enabled)
{
    const auto flags = IrqSpinLockAcquire(&g_synLock);
    EmitTapSteps(g_synTap.SetEnabled(enabled));
    g_synGesture.SetTapDrag(false);
    PublishTapDeadline();
    IrqSpinLockRelease(&g_synLock, flags);
}
bool MouseTouchpadTap() { return g_synTap.Config().enabled; }

void MouseTouchpadSetNatural(bool natural)
{
    const auto flags = IrqSpinLockAcquire(&g_synLock);
    auto cfg = g_synGesture.Config();
    cfg.naturalScroll = natural;
    g_synGesture.SetConfig(cfg);
    IrqSpinLockRelease(&g_synLock, flags);
}

MouseTouchpadStatus MouseTouchpadGetStatus()
{
    MouseTouchpadStatus st{};
    st.wanted = g_synWanted;
    st.active = g_synActive;
    st.reason = g_synReason;
    st.identified = g_synId.valid;
    st.major = g_synId.major; st.minor = g_synId.minor; st.model = g_synId.model;
    st.caps = g_synCaps.caps; st.ext = g_synCaps.ext; st.ext0c = g_synCaps.ext0c;
    st.clickpad = g_synCaps.clickpad();
    st.mode = g_synMode; st.modeReadback = g_synModeReadback;
    st.packetLen = g_packetLen; st.relativeId = g_mouseId;
    st.packets = g_synPackets; st.resyncs = g_synResyncs; st.scrollTicks = g_synScrollTicks;
    for (int i = 0; i < 4; ++i) st.fingers[i] = g_synGesture.fingerHistogram[i];
    st.natural = g_synGesture.Config().naturalScroll;
    st.agm = g_synAgm;
    st.scrollLowZ = g_synGesture.scrollTicksLowZ;
    st.scrollRejected = g_synGesture.scrollRejected;
    st.gestureResets = g_synGesture.resets;
    st.speed = uint32_t(g_synGesture.Config().speedPercent);
    st.tap = g_synTap.Config().enabled;
    st.tapLeft = g_synTap.tapsLeft; st.tapRight = g_synTap.tapsRight;
    st.tapDrags = g_synTap.drags; st.tapDoubles = g_synTap.doubleTaps;
    st.tapRejectedPhysical = g_synTap.rejectedPhysical;
    st.tapRejectedPalm = g_synTap.rejectedPalm;
    st.tapRejectedPhantom = g_synTap.rejectedPhantom;
    st.clicksLeft = g_synGesture.clicksLeft; st.clicksRight = g_synGesture.clicksRight;
    st.clicksRightArea = g_synGesture.clicksRightArea; st.clicksTwoFinger = g_synGesture.clicksTwoFinger;
    st.clicksThumbLeft = g_synGesture.clicksThumbLeft;
    st.clicksStalePos = g_synGesture.clicksStalePos; st.clicksNoPos = g_synGesture.clicksNoPos;
    st.xMin = g_synRange.xMin; st.xMax = g_synRange.xMax; st.yMin = g_synRange.yMin; st.yMax = g_synRange.yMax;
    st.rangeReportedMax = g_synRange.reportedMax; st.rangeReportedMin = g_synRange.reportedMin;
    st.softButtons = g_synGesture.Config().softButtons;
    st.buttonTopY = g_synGesture.Config().buttonTopY; st.buttonSplitX = g_synGesture.Config().buttonSplitX;
    st.traceOn = g_synTraceOn; st.traceTotal = g_synTraceTotal;
    st.rawValid = g_synRawValid;
    for (int i = 0; i < MouseTouchpadRawReplies; ++i) for (int j = 0; j < 3; ++j) st.raw[i][j] = g_synRaw[i][j];
    return st;
}

void MouseTouchpadTraceStart()
{
    const auto flags = IrqSpinLockAcquire(&g_synLock);
    g_synTraceTotal = 0;
    g_synTraceStartMs = ApicTickCount();
    g_synTraceOn = true;
    IrqSpinLockRelease(&g_synLock, flags);
}

MouseTouchpadTraceView MouseTouchpadTraceStop()
{
    // Taking the lock orders this after any packet being recorded; with the
    // trace off nothing writes the ring, so the caller may read it unlocked.
    const auto flags = IrqSpinLockAcquire(&g_synLock);
    g_synTraceOn = false;
    MouseTouchpadTraceView v{};
    v.entries = g_synTrace;
    v.capacity = MouseTouchpadTraceCapacity;
    v.total = g_synTraceTotal;
    v.count = v.total < v.capacity ? v.total : v.capacity;
    v.first = v.total < v.capacity ? 0 : v.total % v.capacity;
    IrqSpinLockRelease(&g_synLock, flags);
    return v;
}

} // namespace brook

// keyboard.cpp — compiled with -mgeneral-regs-only (ISR file).
#include "keyboard.h"
#include "keyboard_layout.h"
#include "vfs.h"
#include "input.h"
#include "idt.h"
#include "apic.h"
#include "serial.h"
#include "tty.h"
#include "kprintf.h"
#include "portio.h"
#include "irq_wrapper.h"
#include "ps2_keyboard_config.h"

namespace brook {

static uint32_t g_keyboardLayout = BROOK_KEYBOARD_US;
static uint32_t g_keyboardDefault = BROOK_KEYBOARD_US;

uint32_t KbdLayout() { return __atomic_load_n(&g_keyboardLayout, __ATOMIC_ACQUIRE); }
uint32_t KbdDefaultLayout() { return g_keyboardDefault; }
void KbdSetLayout(uint32_t layout) { __atomic_store_n(&g_keyboardLayout, layout, __ATOMIC_RELEASE); }

void KbdLoadDefaultLayout()
{
    if (auto* vnode = VfsOpen("/etc/keyboard-layout", 0)) {
        char value[8]{};
        uint64_t offset = 0;
        const int size = VfsRead(vnode, value, sizeof(value) - 1, &offset);
        VfsClose(vnode);
        if (size > 0) g_keyboardDefault = brook_keyboard_layout_parse(value, BROOK_KEYBOARD_US);
    }
    KbdSetLayout(g_keyboardDefault);
}

extern "C" uint16_t KbdTranslateKey(uint8_t scan, uint8_t modifiers)
{
    return KeyboardCodepoint(KbdLayout(), scan, modifiers);
}

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
static volatile bool g_ctrlHeld  = false;
static volatile bool g_altHeld   = false;
static volatile bool g_altGrHeld = false;
static volatile bool g_superHeld = false;
static volatile bool g_capsLock  = false;
static volatile bool g_e0Prefix  = false; // E0-prefixed extended key pending

// Probe counters are sampled outside IRQ context; never print from this path.
// Do not discard the first real keys during firmware handoff.
static uint32_t g_probeIrqs=0, g_probeBytes=0;
static uint8_t g_probeStatus=0, g_probeByte=0;
KbdProbeStats KbdProbeSnapshot() {
    return {__atomic_load_n(&g_probeIrqs,__ATOMIC_RELAXED),
            __atomic_load_n(&g_probeBytes,__ATOMIC_RELAXED),
            __atomic_load_n(&g_probeStatus,__ATOMIC_RELAXED),
            __atomic_load_n(&g_probeByte,__ATOMIC_RELAXED)};
}

// SysRq-style panic: Ctrl+ScrollLock fires a diagnostic kernel panic.
// ScrollLock = scancode 0x46.  Only triggers on key-press (not release).
static constexpr uint8_t SC_SCROLL_LOCK = 0x46;

extern "C" void KernelPanic(const char* fmt, ...);
extern "C" void SchedulerDumpHang();  // BRO-176 non-destructive hang dump (scheduler.cpp)

// ---------------------------------------------------------------------------
// IRQ1 interrupt handler
// ---------------------------------------------------------------------------

extern "C" void KbdIrqHandlerInner(void)
{
    // Check that OBF is set and the byte is NOT from the auxiliary (mouse) port.
    // If bit 5 (AUXB) is set, this is mouse data — don't consume it here.
    __atomic_add_fetch(&g_probeIrqs,1,__ATOMIC_RELAXED);
    uint8_t status = inb(0x64);
    __atomic_store_n(&g_probeStatus,status,__ATOMIC_RELAXED);
    if (!(status & 0x01) || (status & 0x20))
    {
        ApicSendEoi();
        return;
    }

    uint8_t sc = inb(0x60); // read scan code from PS/2 data port
    __atomic_store_n(&g_probeByte,sc,__ATOMIC_RELAXED);
    __atomic_add_fetch(&g_probeBytes,1,__ATOMIC_RELAXED);

    // Filter out keyboard self-test/command response codes that the i8042
    // generates during boot (UEFI handoff). These are NOT valid key scancodes.
    // 0xAA is also Left Shift release, so it must reach the decoder.
    if (sc == 0xFC || sc == 0xFE || sc == 0xFF || sc == 0x00)
    {
        ApicSendEoi();
        return;
    }

    // BRO-176 diag: while Ctrl is held, log the raw scancode of every key so we
    // can identify which scancode (if any) F12 actually emits through QEMU on
    // this host. Lock-free serial (DbgPrintf is compiled out in Release, so use
    // SerialPrintf here — this is a temporary diagnostic). Remove once F12's code
    // is known and the direct binding is corrected.
    if (g_ctrlHeld)
        SerialPrintf("KBD: ctrl+scancode=0x%x e0=%d\n", (unsigned)sc, (int)g_e0Prefix);

    // Ctrl+Shift+ScrollLock → NON-DESTRUCTIVE hang dump (BRO-176). Reuses the
    // ScrollLock scancode (0x46) which is proven to reach this handler, with
    // Shift as the discriminator vs the panic chord below. This must be checked
    // BEFORE the plain Ctrl+ScrollLock panic so the dump wins when Shift is held.
    // (F12 is unreliable: on many laptops it emits an E0 media code, not 0x58.)
    if (sc == SC_SCROLL_LOCK && g_ctrlHeld && g_shiftHeld)
    {
        ApicSendEoi();
        SchedulerDumpHang();
        return;
    }

    // Ctrl+ScrollLock → diagnostic panic (like Linux SysRq+c).
    // Fires from IRQ context so it works even when all threads are deadlocked.
    if (sc == SC_SCROLL_LOCK && g_ctrlHeld)
    {
        ApicSendEoi();
        KernelPanic("Manual panic triggered (Ctrl+ScrollLock)");
    }

    // BRO-176: Ctrl+F12 → same non-destructive hang dump, as a convenience on
    // keyboards where F12 emits the plain 0x58 scancode (desktops). Harmless if
    // F12 never reaches here (laptops); use Ctrl+Shift+ScrollLock there.
    if (sc == 0x58 /* F12 */ && g_ctrlHeld)
    {
        ApicSendEoi();
        SchedulerDumpHang();
        return;
    }

    // Handle E0 prefix (extended keys: arrows, Home, End, etc.)
    if (sc == 0xE0)
    {
        g_e0Prefix = true;
        ApicSendEoi();
        return;
    }

    bool release = (sc & 0x80) != 0;
    uint8_t key  = sc & 0x7F;

    // Map E0-prefixed scancodes to synthetic extended codes
    bool isExtended = false;
    if (g_e0Prefix)
    {
        g_e0Prefix = false;
        isExtended = true;
        switch (key)
        {
        case 0x48: key = SC_EXT_UP;     break;
        case 0x50: key = SC_EXT_DOWN;   break;
        case 0x4B: key = SC_EXT_LEFT;   break;
        case 0x4D: key = SC_EXT_RIGHT;  break;
        case 0x47: key = SC_EXT_HOME;   break;
        case 0x4F: key = SC_EXT_END;    break;
        case 0x52: key = SC_EXT_INSERT; break;
        case 0x53: key = SC_EXT_DELETE; break;
        case 0x49: key = SC_EXT_PGUP;   break;
        case 0x51: key = SC_EXT_PGDN;   break;
        case 0x1D: // R-Ctrl
            g_ctrlHeld = !release;
            ApicSendEoi();
            return;
        case 0x38: // R-Alt
            g_altGrHeld = !release;
            key = 100; // evdev KEY_RIGHTALT
            break;
        case 0x5B: // Left Super (GUI)
        case 0x5C: // Right Super (GUI)
            // Published (not just tracked) so the desktop can act on a tap.
            g_superHeld = !release;
            key = key == 0x5B ? SC_LSUPER : SC_RSUPER;
            break;
        default:
            // Unknown E0 key — ignore
            ApicSendEoi();
            return;
        }
    }

    // Track modifier state (non-extended keys only).
    if (!isExtended)
    {
        if (key == 0x2A || key == 0x36)      // L-Shift or R-Shift
            g_shiftHeld = !release;
        else if (key == 0x1D)                // L-Ctrl
            g_ctrlHeld = !release;
        else if (key == 0x38)                // L-Alt
            g_altHeld = !release;
        else if (key == 0x3A && !release)    // Caps Lock (toggle on press)
            g_capsLock = !g_capsLock;
    }

    // Build modifier bitmask AFTER state update so modifier key events
    // carry the resulting state (e.g. shift press has shift in mods).
    uint8_t mods = 0;
    if (g_shiftHeld) mods |= INPUT_MOD_LSHIFT;
    if (g_ctrlHeld)  mods |= INPUT_MOD_CTRL;
    if (g_altHeld || (g_altGrHeld && KbdLayout() == BROOK_KEYBOARD_US)) mods |= INPUT_MOD_ALT;
    if (g_altGrHeld) mods |= INPUT_MOD_ALTGR;
    if (g_superHeld) mods |= INPUT_MOD_SUPER;
    if (g_capsLock)  mods |= INPUT_MOD_CAPSLOCK;

    InputEvent ev{};
    ev.type = release ? InputEventType::KeyRelease : InputEventType::KeyPress;
    ev.scanCode = key;
    ev.modifiers = mods;
    if (!release && !isExtended) {
        ev.textCodepoint = KbdTranslateKey(key, mods);
        if (ev.textCodepoint < 128) ev.ascii = char(ev.textCodepoint);
        KbdPushCodepoint(ev.textCodepoint);
    }
    InputDevicePush(&g_kbdInputDev, ev);
    InputWakeWaiters();

    // Send EOI to LAPIC.
    ApicSendEoi();
}

// Naked ISR wrapper — handles SWAPGS + GPR save/restore around the inner handler.
IRQ_NAKED_HANDLER(KbdIrqHandler, KbdIrqHandlerInner)

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Tracks whether KbdInit() has been called.
static bool g_kbdInitialized = false;

void KbdInit()
{
    if (g_kbdInitialized) return;
    struct InterruptGuard {
        uint64_t flags;
        InterruptGuard() { __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory"); }
        ~InterruptGuard() { if(flags & (1ull<<9)) __asm__ volatile("sti":::"memory"); }
    } guard;
    // No 8042 at all (legacy-free boards): the status port floats to 0xff.
    // Every wait below would otherwise run to its full bound.
    if (inb(0x64) == 0xff) { KPuts("KBD: no PS/2 controller (status 0xff)\n"); return; }
    // Waits are bounded by iterations and by elapsed time: where firmware
    // emulates the 8042 in SMM, one status read can cost microseconds.
    struct Controller {
        uint64_t start = 0;
        bool expired(unsigned i, uint64_t ms) {
            if (!i) start = ApicTickCount();
            return (i & 63) == 63 && ApicTickCount() - start >= ms;
        }
        bool waitWrite() { for(unsigned i=0;i<100000 && !expired(i,50);++i) { if(!(inb(0x64)&2)) return true; __asm__ volatile("pause"); } return false; }
        void command(uint8_t b) { outb(0x64,b); }
        void data(uint8_t b) { outb(0x60,b); }
        void drain() { for(unsigned i=0;i<32 && (inb(0x64)&1);++i) (void)inb(0x60); }
        bool readController(uint8_t& b) {
            for(unsigned i=0;i<100000 && !expired(i,100);++i) {
                const auto status=inb(0x64);
                if(status&1) { const auto value=inb(0x60); if(!(status&0x20)) { b=value; return true; } }
                __asm__ volatile("pause");
            }
            return false;
        }
    } controller;
    Ps2KeyboardConfig config;
    const bool configured=ConfigurePs2Keyboard(controller,config);
    KPrintf("KBD: controller old=0x%x requested=0x%x readback=0x%x stage=%u\n",
            config.before,config.requested,config.after,config.stage);
    if(!configured) { KPuts("KBD: controller configuration failed; IRQ1 remains masked\n"); return; }
    // Flush any stale bytes in the PS/2 output buffer.
    // UEFI firmware may leave residual scan codes during the handoff.
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 0x01)) break;
        inb(0x60);
    }

    // Clear the legacy ring buffer in case any stale data was pushed
    // before init (e.g. from early ISR setup or firmware handoff).
    g_kbdHead = 0;
    g_kbdTail = 0;

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

extern "C" void KbdPushCodepoint(uint16_t cp)
{
    char bytes[3];
    const unsigned count = KeyboardUtf8(cp, bytes);
    for (unsigned i = 0; i < count; ++i) KbdPushChar(bytes[i]);
}

void KbdPushChar(char c)
{
    BufPush(c);
}

} // namespace brook

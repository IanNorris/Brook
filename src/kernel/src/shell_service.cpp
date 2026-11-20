#include "shell_service.h"

#include "chrome_shell_model.h"
#include "chrome_shell_wire.h"
#include "compositor.h"
#include "desktop_shell_surfaces.h"
#include "input.h"
#include "desktop_identity.h"
#include "memory/heap.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "string.h"
#include "sync/kmutex.h"
#include "syscall_internal.h"
#include "user_copy.h"

inline void* operator new(size_t, void* storage) noexcept { return storage; }

namespace brook {
namespace {

constexpr unsigned KindCount = unsigned(WmShellSurfaceKind::Notification) + 1;

struct Geometry {
    WmShellSurface surface;
    uint32_t width, height;
    uint64_t style;
};

struct Submission {
    Geometry geometry;
    uint64_t key, sequence;
    uint32_t count, textBytes;
    ChromeCmd* cmds;
    char* text;
    uint32_t scale;
};

struct Slot {
    ChromeShellModel model;
    ChromeShellConfig style;
    Geometry requestedGeometry, offeredGeometry;
    uint64_t requested, offered, lastSample, lastInput;
    uint64_t visibleFrom;
    bool visible;
    int32_t lastMouseX, lastMouseY;
    uint32_t offeredScale;
    Submission* staged;
    // Compositor thread only. Swap staged lists in before drawing a frame.
    Submission* active;
};

KMutex g_lock{};
struct ShellStorage {
    Slot slots[KindCount];
    ChromeShellModel capture;
    ShellSnapshot receive;
};
// Model defaults are populated when the service first starts. Keeping these
// screen-sized buffers in BSS leaves the guarded boot kernel size unchanged.
alignas(ShellStorage) unsigned char g_storage[sizeof(ShellStorage)];
ShellStorage* g_shell = nullptr;
Process* g_waiter = nullptr;
uint16_t g_pid = 0;
uint64_t g_incarnation = 0;
uint64_t g_sequence = 0;
uint64_t g_ownerCheck = 0;
bool g_registered = false;
bool g_adoptionPending = false;
ChromeTrayState g_tray{}, g_trayStaged{};
TrayInput g_trayInput[32]{};
uint32_t g_trayHead = 0, g_trayCount = 0;
bool g_trayPending = false;


struct Lock {
    Lock() { KMutexLock(&g_lock); }
    ~Lock() { KMutexUnlock(&g_lock); }
};

bool SameGeometry(const Geometry& a, const Geometry& b)
{
    return a.width == b.width && a.height == b.height && a.style == b.style &&
           a.surface.kind == b.surface.kind && a.surface.material == b.surface.material &&
           a.surface.x == b.surface.x && a.surface.y == b.surface.y &&
           a.surface.w == b.surface.w && a.surface.h == b.surface.h;
}

const ChromeFontSet& Fonts(uint32_t scale)
{
    return scale == 200 ? g_chromeFonts200 : scale == 150 ? g_chromeFonts150 : g_chromeFonts100;
}

void WakeReader()
{
    if (!g_waiter) return;
    __atomic_store_n(&g_waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
    SchedulerUnblock(g_waiter);
}

bool SameOwner(const Process& proc)
{
    return g_pid == proc.pid && g_incarnation == proc.incarnation;
}

int64_t Register(Process& proc)
{
    {
        Lock lock;
        if (SameOwner(proc)) return 0;
        if (g_pid) return -16; // Previous owner has not yet been retired by the compositor.
        if (!g_shell) g_shell = new (g_storage) ShellStorage{};
        g_pid = proc.pid;
        g_incarnation = proc.incarnation;
        __atomic_store_n(&g_registered, true, __ATOMIC_RELEASE);
    }
    CompositorWake();
    return 0;
}

int64_t Receive(Process& proc, uint64_t exchange, uint64_t modelAddress)
{
    ShellExchange offer{};
    for (;;) {
        {
            Lock lock;
            if (!SameOwner(proc)) return -1;
            Slot* slot = nullptr;
            for (auto& candidate : g_shell->slots)
                if (candidate.requested > candidate.offered &&
                    (!slot || candidate.requested < slot->requested)) slot = &candidate;
            if (slot) {
                g_shell->receive.config = slot->style;
                g_shell->receive.model = slot->model;
                offer = {slot->requested, unsigned(slot - g_shell->slots), ShellServiceVersion};
                slot->offered = offer.sequence;
                slot->offeredGeometry = slot->requestedGeometry;
                slot->offeredScale = slot->style.metrics.percent;
                break;
            }
            if (HasPendingSignals()) return -4;
            g_waiter = &proc;
            __atomic_store_n(&proc.pendingWakeup, 0, __ATOMIC_RELEASE);
        }
        SchedulerBlockInterruptible(&proc);
        {
            Lock lock;
            g_waiter = nullptr;
        }
    }
    if (CopyToUser(modelAddress, &g_shell->receive, sizeof(g_shell->receive)) &&
        CopyToUser(exchange, &offer, sizeof(offer))) return 0;
    // Let a retry receive the same work if the destination faulted.
    Lock lock;
    auto& slot = g_shell->slots[offer.kind];
    if (slot.offered == offer.sequence) slot.offered = 0;
    return -14;
}

int64_t Present(Process& proc, uint64_t exchange, uint64_t address)
{
    ShellExchange offer{};
    uint32_t sizes[3];
    if (!CopyFromUser(&offer, exchange, sizeof(offer)) ||
        !CopyFromUser(sizes, address, sizeof(sizes))) return -14;
    const auto count = sizes[0], textBytes = sizes[1], assetWords = sizes[2];
    if (offer.version != ShellServiceVersion || offer.kind >= KindCount || !offer.sequence ||
        !count || count > ShellCommandLimit || textBytes > ShellTextLimit ||
        assetWords > ShellAssetLimit) return -22;
    Geometry geometry{};
    uint32_t scale = 0;
    {
        Lock lock;
        if (!SameOwner(proc)) return -1;
        auto& slot = g_shell->slots[offer.kind];
        if (slot.offered != offer.sequence) return -116;
        geometry = slot.offeredGeometry;
        scale = slot.offeredScale;
    }

    const uint64_t commandBytes = uint64_t(count) * sizeof(ChromeCmd);
    const uint64_t dataBytes = uint64_t(assetWords) * sizeof(uint32_t);
    auto* result = static_cast<Submission*>(kmalloc(sizeof(Submission) + commandBytes + dataBytes + textBytes));
    auto* wire = static_cast<ShellWireCommand*>(kmalloc(uint64_t(count) * sizeof(ShellWireCommand)));
    if (!result || !wire) {
        kfree(result);
        kfree(wire);
        return -12;
    }
    auto* commands = reinterpret_cast<ChromeCmd*>(result + 1);
    auto* assets = reinterpret_cast<uint32_t*>(commands + count);
    auto* text = reinterpret_cast<char*>(assets + assetWords);
    int64_t error = 0;
    if (!CopyFromUser(wire, address + offsetof(ShellWireList, commands), uint64_t(count) * sizeof(*wire)) ||
        (textBytes && !CopyFromUser(text, address + offsetof(ShellWireList, text), textBytes)) ||
        (assetWords && !CopyFromUser(assets, address + offsetof(ShellWireList, assets), dataBytes))) error = -14;
    for (uint32_t i = 0; !error && i < count; ++i)
        if (!DecodeShellCommand(wire[i], text, textBytes, assets, assetWords, commands[i])) error = -22;
    kfree(wire);
    if (!error) {
        ChromeDisplayList list(commands, count, text, textBytes, count, textBytes);
        const uint64_t key = ChromeListKey(list, geometry.style ^ (uint64_t(offer.kind) << 32));
        *result = {geometry, key, offer.sequence, count, textBytes, commands, text, scale};
        Lock lock;
        auto& slot = g_shell->slots[offer.kind];
        if (!SameOwner(proc) || slot.offered != offer.sequence ||
            !SameGeometry(geometry, slot.requestedGeometry)) error = -116;
        else {
            auto* previous = slot.staged;
            slot.staged = result;
            result = previous;
            __atomic_store_n(&g_adoptionPending, true, __ATOMIC_RELEASE);
        }
    }
    kfree(result);
    if (!error) CompositorWake();
    return error;
}

bool TrayChild(const Process& proc)
{
    return proc.euid == DesktopUid && g_pid && proc.parentPid == g_pid;
}

template<size_t N> bool TrayString(const char (&text)[N])
{
    return text[N - 1] == 0;
}

bool ValidTray(const ChromeTrayState& tray)
{
    if (tray.itemCount > TrayItemLimit || tray.menuCount > TrayMenuLimit ||
        tray.notice.actionCount > NoticeActionLimit) return false;
    bool owner = tray.menuCount == 0 && tray.menuOwner == 0;
    for (uint32_t i = 0; i < tray.itemCount; ++i) {
        const auto& item = tray.items[i];
        if (!item.id || !TrayString(item.title) || !TrayString(item.tooltip)) return false;
        owner |= tray.menuCount && item.id == tray.menuOwner;
        for (uint32_t j = 0; j < i; ++j) if (tray.items[j].id == item.id) return false;
    }
    if (!owner || !TrayString(tray.notice.app) || !TrayString(tray.notice.title) ||
        !TrayString(tray.notice.body)) return false;
    for (uint32_t i = 0; i < tray.menuCount; ++i)
        if (!TrayString(tray.menu[i].label)) return false;
    for (uint32_t i = 0; i < tray.notice.actionCount; ++i)
        if (!TrayString(tray.notice.actions[i])) return false;
    return true;
}

int64_t TrayPublish(const Process& proc, uint64_t address)
{
    auto* tray = static_cast<ChromeTrayState*>(kmalloc(sizeof(ChromeTrayState)));
    if (!tray) return -12;
    int64_t result = 0;
    if (!CopyFromUser(tray, address, sizeof(ChromeTrayState))) result = -14;
    else if (!ValidTray(*tray)) result = -22;
    else {
        Lock lock;
        if (!TrayChild(proc)) result = -1;
        else if (memcmp(tray, &g_trayStaged, sizeof(ChromeTrayState))) {
            g_trayStaged = *tray;
            g_trayPending = true;
            __atomic_store_n(&g_adoptionPending, true, __ATOMIC_RELEASE);
        }
    }
    kfree(tray);
    if (!result) CompositorWake();
    return result;
}

int64_t TrayNextInput(const Process& proc, uint64_t address)
{
    TrayInput input{};
    {
        Lock lock;
        if (!TrayChild(proc)) return -1;
        if (!g_trayCount) return -11;
        input = g_trayInput[g_trayHead];
        g_trayHead = (g_trayHead + 1) % 32;
        --g_trayCount;
    }
    return CopyToUser(address, &input, sizeof(input)) ? 0 : -14;
}

} // namespace

const ChromeTrayState& ShellServiceTrayState() { return g_tray; }

void ShellServiceQueueTrayInput(const TrayInput& input)
{
    Lock lock;
    if (!g_pid || g_trayCount == 32) return;
    g_trayInput[(g_trayHead + g_trayCount) % 32] = input;
    ++g_trayCount;
}

void ShellServiceBeginFrame(uint64_t now)
{
    if (!__atomic_load_n(&g_registered, __ATOMIC_ACQUIRE)) return;
    const bool checkOwner = now >= g_ownerCheck;
    if (!checkOwner && !__atomic_load_n(&g_adoptionPending, __ATOMIC_ACQUIRE)) return;

    Submission* retired[KindCount * 2]{};
    unsigned count = 0;
    {
        Lock lock;
        if (checkOwner) {
            g_ownerCheck = now + 50;
            ProcessSnapshot owner{};
            if (g_pid && (!SchedulerSnapshotProcess(g_pid, &owner) || owner.incarnation != g_incarnation ||
                         owner.state == ProcessState::Terminated)) {
                memset(&g_tray, 0, sizeof(g_tray));
                memset(&g_trayStaged, 0, sizeof(g_trayStaged));
                g_trayPending = false;
                g_trayHead = g_trayCount = 0;
                g_pid = 0;
                g_incarnation = 0;
                __atomic_store_n(&g_registered, false, __ATOMIC_RELEASE);
                for (auto& slot : g_shell->slots) {
                    retired[count++] = slot.active;
                    retired[count++] = slot.staged;
                    slot.active = slot.staged = nullptr;
                    slot.requested = slot.offered = 0;
                    slot.visible = false;
                }
            }
        }
        if (__atomic_exchange_n(&g_adoptionPending, false, __ATOMIC_ACQ_REL)) {
            if (g_trayPending) {
                g_tray = g_trayStaged;
                g_trayPending = false;
            }
            for (auto& slot : g_shell->slots) if (slot.staged) {
                retired[count++] = slot.active;
                slot.active = slot.staged;
                slot.staged = nullptr;
            }
        }
    }
    for (unsigned i = 0; i < count; ++i) kfree(retired[i]);
}

static uint64_t CaptureModel(const WmShellSurface& surface, uint32_t width, uint32_t height,
                             uint64_t now, int32_t mx, int32_t my, bool visible)
{
    if (!__atomic_load_n(&g_registered, __ATOMIC_ACQUIRE)) return 0;
    auto& slot = g_shell->slots[unsigned(surface.kind)];
    const auto config = WmCaptureShellConfig();
    uint64_t style = (WmBackdropStyleVersion() * 0x100000001b3ull) ^
                           (uint64_t(config.metrics.percent) << 32) ^
                           (uint64_t(config.glass) << 16) ^ config.materialOpacity;
    // Theme settings include custom RGB and can change without a geometry
    // change. Reject lists built with a different palette.
    const auto* palette = reinterpret_cast<const unsigned char*>(&config.palette);
    for (size_t i = 0; i < sizeof(config.palette); ++i) style = (style ^ palette[i]) * 0x100000001b3ull;
    const Geometry geometry{surface, width, height, style};
    const bool changedGeometry = !slot.requested || !SameGeometry(slot.requestedGeometry, geometry);
    const auto input = InputLatestSequence();
    // Keep input-driven changes immediate. Sample clock and telemetry at the
    // existing overlay recheck cadence while the pointer and input are idle.
    if (changedGeometry || slot.visible != visible || input != slot.lastInput || mx != slot.lastMouseX || my != slot.lastMouseY ||
        now < slot.lastSample || now - slot.lastSample >= 100) {
        if (WmCaptureShellModel(surface, width, height, now, mx, my, g_shell->capture) &&
            (changedGeometry || memcmp(&slot.model, &g_shell->capture, sizeof(g_shell->capture)))) {
            Lock lock;
            slot.model = g_shell->capture;
            slot.style = config;
            slot.requestedGeometry = geometry;
            slot.requested = ++g_sequence;
            WakeReader();
        }
        slot.lastSample = now;
        slot.lastInput = input;
        slot.lastMouseX = mx;
        slot.lastMouseY = my;
    }
    // Use the hidden launcher list immediately when opening captured the
    // same model. If opening changed its model, wait for that publication
    // so a previous session's scroll, hover or confirmation cannot flash.
    if (visible && !slot.visible) slot.visibleFrom = slot.requested;
    slot.visible = visible;
    return visible && slot.active && SameGeometry(slot.active->geometry, geometry) &&
           slot.active->sequence >= slot.visibleFrom &&
           slot.active->scale == config.metrics.percent ? slot.active->key : 0;
}

uint64_t ShellServiceCapture(const WmShellSurface& surface, uint32_t width, uint32_t height,
                             uint64_t now, int32_t mx, int32_t my)
{
    return CaptureModel(surface, width, height, now, mx, my, true);
}

bool ShellServiceWarmLauncher(uint32_t width, uint32_t height, uint64_t now)
{
    if (!__atomic_load_n(&g_registered, __ATOMIC_ACQUIRE)) return false;
    const auto& launcher = g_shell->slots[unsigned(WmShellSurfaceKind::Launcher)];
    if (launcher.requested && !launcher.visible && now >= launcher.lastSample &&
        now - launcher.lastSample < 100 && launcher.requestedGeometry.width == width &&
        launcher.requestedGeometry.height == height) return true;
    WmShellSurface surface{WmShellSurfaceKind::Launcher, 0, 0, 0, 0, true};
    WmLauncherGetRect(width, height, &surface.x, &surface.y, &surface.w, &surface.h);
    (void)CaptureModel(surface, width, height, now, -1, -1, false);
    return true;
}

void ShellServiceEndCapture(uint32_t kindMask)
{
    if (!__atomic_load_n(&g_registered, __ATOMIC_ACQUIRE)) return;
    for (unsigned kind = 0; kind < KindCount; ++kind) {
        auto& slot = g_shell->slots[kind];
        if ((kindMask & (1u << kind)) || !slot.requested) continue;
        Submission* active;
        Submission* staged;
        {
            Lock lock;
            active = slot.active;
            staged = slot.staged;
            slot.active = slot.staged = nullptr;
            slot.requested = slot.offered = 0;
            slot.visible = false;
        }
        kfree(active);
        kfree(staged);
    }
}

void ShellServiceDraw(const WmShellSurface& surface, uint32_t* pixels, uint32_t stride,
                      uint32_t width, uint32_t height)
{
    const auto* list = g_shell->slots[unsigned(surface.kind)].active;
    if (!list || !SameGeometry(list->geometry, {surface, width, height, list->geometry.style})) return;
    if (surface.x < 0 || surface.y < 0 || uint64_t(surface.x) + surface.w > width ||
        uint64_t(surface.y) + surface.h > height) return;
    ChromeDisplayList view(list->cmds, list->count, list->text, list->textBytes, list->count, list->textBytes);
    ChromeExecute(view, {pixels + uint64_t(surface.y) * stride + surface.x, stride, surface.w, surface.h,
                         surface.x, surface.y}, Fonts(list->scale));
}

int64_t ShellServiceSyscallInvoke(uint64_t op, uint64_t exchange, uint64_t payload,
                                 uint64_t bytes, uint64_t reserved1, uint64_t reserved2)
{
    auto* proc = ProcessCurrent();
    if (!proc || proc->isThread) return -1;
    const bool tray = op == uint64_t(ShellServiceOp::TrayPublish) ||
                      op == uint64_t(ShellServiceOp::TrayNextInput);
    if (tray ? proc->euid != DesktopUid : proc->euid != 0) return -1;
    // A mounted QEMU boot volume reports /boot. The guarded initrd keeps
    // the FAT directory's canonical /BOOT spelling in the executable path.
    if (strcmp(proc->exePath, "/boot/CWSHELL.ELF") && strcmp(proc->exePath, "/BOOT/CWSHELL.ELF")) {
        if (op == uint64_t(ShellServiceOp::Register))
            SerialPrintf("CLEARWATER_SHELL_REJECT pid=%u executable=%s\n", proc->pid, proc->exePath);
        return -1;
    }
    if (reserved1 || reserved2) return -22;
    if (tray) {
        if (exchange || !payload) return -22;
        {
            Lock lock;
            if (!TrayChild(*proc)) return -1;
        }
        KernelOperationGuard guard(proc);
        if (op == uint64_t(ShellServiceOp::TrayPublish))
            return bytes == sizeof(ChromeTrayState) ? TrayPublish(*proc, payload) : -22;
        return bytes == sizeof(TrayInput) ? TrayNextInput(*proc, payload) : -22;
    }
    if (op != uint64_t(ShellServiceOp::Register) && (!exchange ||
        payload > (~uint64_t{0}) - sizeof(ShellWireList) || !payload)) return -22;
    KernelOperationGuard guard(proc);
    switch (ShellServiceOp(op)) {
    case ShellServiceOp::Register:
        if (exchange || payload || bytes != ShellServiceVersion) return -22;
        return Register(*proc);
    case ShellServiceOp::Receive:
        if (bytes != sizeof(ShellSnapshot)) return -22;
        return Receive(*proc, exchange, payload);
    case ShellServiceOp::Present:
        if (bytes != sizeof(ShellWireList)) return -22;
        return Present(*proc, exchange, payload);
    case ShellServiceOp::TrayPublish:
    case ShellServiceOp::TrayNextInput:
        return -22;
    }
    return -22;
}

} // namespace brook

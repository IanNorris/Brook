// Taskbar, tray indicators, volume popup and tray tooltips: state caches,
// model snapshots and hit tests. The layout and display list builders are
// freestanding (chrome_shell.cpp). Part of window.cpp's translation unit (it
// shares the WM's window list and launch state). Include only from
// window.cpp.

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

#include "desktop_telemetry_runtime.inc"

using TaskbarLayout = ChromeTaskbarLayout;

enum class WmHudMode : uint8_t { Off, Compact, Panel };
static WmHudMode g_hudMode;
static TaskbarPopupPin g_taskbarPopupPin;

static uint32_t WmVisibleTrayMenuOwner()
{
    const auto& tray = ShellServiceTrayState();
    if (!tray.menuOwner || !tray.menuCount) return 0;
    for (uint32_t i = 0; i < tray.itemCount; ++i)
        if (tray.items[i].id == tray.menuOwner) return tray.menuOwner;
    return 0;
}

void WmTaskbarPopupForgetMenu() { g_taskbarPopupPin.RetainAppMenu(0); }

bool WmTaskbarPopupPinned()
{
    g_taskbarPopupPin.RetainAppMenu(WmVisibleTrayMenuOwner());
    return g_taskbarPopupPin.Any();
}

// Debug-bridge agent indicator cache (see WmAgentIndicatorRefresh).
static uint8_t g_agentIndicatorState = 0; // BridgePresenceState
static uint32_t g_agentIndicatorPeer = 0;
static uint64_t g_agentIndicatorSilentMs = 0;

static TaskbarLayout GetTaskbarLayout(uint32_t screenW, uint32_t screenH)
{
    const auto clock = RtcReadTaskbarClock();
    // Build label: the payload name (e.g. "dell-int51-20261119") when the
    // boot has one, else the short commit. The tooltip has the detail.
    char buildText[48]{};
    if (g_buildChip) {
        if (g_buildPayloadId[0]) WmStrCopy(buildText, g_buildPayloadId, sizeof(buildText));
        else {
            WmStrCopy(buildText, BuildGitHash(), sizeof(buildText));
            for (char* p = buildText; *p; ++p) if (*p == '+') { *p = 0; break; }
        }
    }
    int ids[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity], total = 0, focused = 0;
    const char* labels[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity];
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        const Window& w = g_windows[i];
        if (!w.proc || !w.visible || !w.focusable || w.docked) continue; // docked: the dock's
        if (w.focused) focused = total;
        labels[total] = WindowDisplayTitle(w);
        ids[total++] = static_cast<int>(i);
    }
    SpinLockAcquire(&g_launchLock);
    for (unsigned i = 0; i < LaunchFeedbackTable::Capacity; ++i) {
        if (!g_launches.entries[i].present()) continue;
        if (g_taskbarPageTarget == -10 - static_cast<int>(i)) focused = total;
        labels[total] = nullptr; // launch feedback: icon only
        ids[total++] = -10 - static_cast<int>(i);
    }
    SpinLockRelease(&g_launchLock);
    return ChromeTaskbarLayoutFor(WmChromeStyle(), screenW, screenH, clock,
                                  __atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE) != 0,
                                  WmGetTaskbarMetrics(), ids, total, focused, buildText, g_hudMode == WmHudMode::Compact,
                                  labels, ShellServiceTrayState().itemCount);
}

// Network indicator. The compositor thread refreshes this cache at most once
// per second from already-maintained stack state (primary interface address,
// plus the driver's software failure flag); taskbar rendering only reads it.
// No NIC register is touched here. Address 0 means disconnected.
static uint32_t g_netIndicatorAddress = 0;
static uint64_t g_netIndicatorTick = 0;
static bool g_netIndicatorSampled = false;

bool WmNetworkIndicatorRefresh(uint64_t now)
{
    if (g_netIndicatorSampled && now - g_netIndicatorTick < 1000) return false;
    g_netIndicatorSampled = true;
    g_netIndicatorTick = now;
    uint32_t address = 0;
    if (NetIf* nif = NetGetIf()) {
        address = __atomic_load_n(&nif->ipAddr, __ATOMIC_RELAXED);
        NetDriverSnapshot driver{};
        if (address && nif->snapshot && nif->snapshot(nif, &driver) && driver.failed) address = 0;
    }
    const uint32_t previous = __atomic_exchange_n(&g_netIndicatorAddress, address, __ATOMIC_ACQ_REL);
    if (previous == address) return false;
    const uint8_t* octet = reinterpret_cast<const uint8_t*>(&address);
    if (address) SerialPrintf("WM: network indicator connected %u.%u.%u.%u\n", octet[0], octet[1], octet[2], octet[3]);
    else SerialPuts("WM: network indicator disconnected\n");
    return true;
}

// Debug-bridge agent indicator. The guest sees only its own debug listener:
// heartbeats ("bridge presence beat") from a long-lived MCP bridge attach,
// other framed requests from that peer refresh, and an explicit detach
// clears. Silence beyond the stale threshold shows a warning variant; ten
// minutes of silence clears it. The compositor thread samples once per
// second (one short spinlock); taskbar rendering reads only this cache.
bool WmAgentIndicatorRefresh(uint64_t now)
{
    static uint64_t lastTick = 0;
    static bool sampled = false;
    if (sampled && now - lastTick < 1000) return false;
    sampled = true;
    lastTick = now;
    const BridgePresenceView view = DebugPresenceSnapshot(now);
    const uint8_t state = static_cast<uint8_t>(view.state);
    __atomic_store_n(&g_agentIndicatorPeer, view.peer, __ATOMIC_RELAXED);
    __atomic_store_n(&g_agentIndicatorSilentMs, view.silentMs, __ATOMIC_RELAXED);
    const uint8_t previous = __atomic_exchange_n(&g_agentIndicatorState, state, __ATOMIC_ACQ_REL);
    if (previous == state) return false;
    const uint8_t* o = reinterpret_cast<const uint8_t*>(&view.peer);
    SerialPrintf("WM: agent indicator %s peer=%u.%u.%u.%u silent_ms=%u\n", BridgePresenceStateName(view.state),
                 o[0], o[1], o[2], o[3], static_cast<uint32_t>(view.silentMs));
    return true;
}

static void AgentIndicatorTooltip(char (&text)[48])
{
    const uint8_t state = __atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE);
    const uint32_t peer = __atomic_load_n(&g_agentIndicatorPeer, __ATOMIC_RELAXED);
    auto append = [&](const char* part) {
        const size_t n = strlen(text);
        WmStrCopy(text + n, part, sizeof(text) - n);
    };
    auto number = [&](uint64_t value) {
        char digits[12];
        DesktopNumber(digits, static_cast<unsigned>(value));
        append(digits);
    };
    text[0] = 0;
    if (state == static_cast<uint8_t>(BridgePresenceState::Connected)) {
        append("Agent connected from ");
        const uint8_t* o = reinterpret_cast<const uint8_t*>(&peer);
        for (int i = 0; i < 4; ++i) { number(o[i]); if (i < 3) append("."); }
        return;
    }
    // The guest can tell only that the host went quiet, not why; its own
    // link state is the one cause it can rule in.
    if (!__atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE)) {
        WmStrCopy(text, "Agent unreachable: network down", sizeof(text));
        return;
    }
    const uint64_t seconds = __atomic_load_n(&g_agentIndicatorSilentMs, __ATOMIC_RELAXED) / 1000;
    append("Agent stopped responding ");
    if (seconds < 120) { number(seconds); append(" s ago"); }
    else { number(seconds / 60); append(" min ago"); }
}

// Taskbar volume. The control is the same atomic master gain the Settings app
// reads and writes through BROOK_SYS_AUDIO_CONTROL (AudioOutputControl and
// AudioSetUserOutputControl). The compositor thread copies it into this cache
// once per loop (two relaxed loads, no device access); rendering reads only
// the cache, so a change from either side appears on the next overlay check.
// Popup and drag state are owned by the compositor thread.
static uint32_t g_volumeControl = BROOK_AUDIO_DEFAULT;
static bool g_volumeAvailable = false;
static bool g_volumeSampled = false;
static bool g_volumePopupOpen = false;
static bool g_volumeDragging = false;

bool WmVolumeIndicatorRefresh()
{
    const uint32_t control = AudioOutputControl();
    const bool available = AudioAvailable();
    if (g_volumeSampled && control == g_volumeControl && available == g_volumeAvailable) return false;
    g_volumeSampled = true;
    g_volumeControl = control;
    g_volumeAvailable = available;
    return true;
}

// Volume-change feedback ("pop", src/kernel/assets/README.md). Played once at
// the new level through a short kernel-owned mixer stream, so it follows the
// master volume and mute; policy in volume_feedback.h. Compositor thread only.
extern "C" const uint8_t g_volumePopPcm[], g_volumePopPcmEnd[];
static VolumeFeedback g_volumeFeedback;
static uint32_t g_volumeFeedbackSerial = 0, g_volumeFeedbackControl = 0;
static bool g_volumeFeedbackPrimed = false;
static AudioStreamId g_volumePopStream = 0;
static uint64_t g_volumePopStartMs = 0;

static void WmVolumePopPlay(uint64_t nowMs)
{
    if (!AudioAvailable()) return;
    const auto* pcm = reinterpret_cast<const int16_t*>(g_volumePopPcm);
    const uint32_t frames = static_cast<uint32_t>((g_volumePopPcmEnd - g_volumePopPcm) / MIXER_FRAME_BYTES);
    if (!frames) return;
    if (g_volumePopStream && !AudioMixerReset(g_volumePopStream)) { AudioMixerClose(g_volumePopStream); g_volumePopStream = 0; }
    if (!g_volumePopStream) g_volumePopStream = AudioMixerOpenCue();
    if (!g_volumePopStream) return; // all mixer slots in use: skip the cue
    AudioMixerSubmit(pcm, frames, g_volumePopStream);
    g_volumePopStartMs = nowMs;
}

void WmVolumeFeedbackTick(uint64_t nowMs)
{
    const uint32_t serial = AudioUserOutputControlSerial();
    const uint32_t control = AudioOutputControl();
    if (!g_volumeFeedbackPrimed) {
        g_volumeFeedbackPrimed = true; g_volumeFeedbackSerial = serial; g_volumeFeedbackControl = control;
        return;
    }
    if (serial != g_volumeFeedbackSerial) {
        g_volumeFeedback.Change(g_volumeFeedbackControl, control);
        g_volumeFeedbackSerial = serial;
    }
    g_volumeFeedbackControl = control;
    if (g_volumeFeedback.Tick(nowMs)) WmVolumePopPlay(nowMs);
    // Release the slot once the 80 ms clip has certainly drained.
    if (g_volumePopStream && nowMs - g_volumePopStartMs > 1000) { AudioMixerClose(g_volumePopStream); g_volumePopStream = 0; }
}

static void WmVolumeApply(uint32_t control)
{
    control &= 0x17fu;
    if (control == g_volumeControl || !AudioSetUserOutputControl(control)) return;
    g_volumeControl = AudioOutputControl();
    SerialPrintf("WM: volume %u%s\n", brook_audio_volume(g_volumeControl),
                 (g_volumeControl & BROOK_AUDIO_MUTED) ? " muted" : "");
}

static TaskbarVolumePopupLayout WmVolumePopupLayout(uint32_t screenW, uint32_t screenH)
{
    const TaskbarLayout t = GetTaskbarLayout(screenW, screenH);
    return TaskbarVolumePopup(WmDesktopMetrics(), t.volume, t.y, static_cast<int>(screenW), ChromeVolumeValueWidth(WmChromeStyle()));
}

static bool WmVolumeIconContains(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    return GetTaskbarLayout(screenW, screenH).volume.contains(mx, my);
}

bool WmVolumePopupVisible() { return g_volumePopupOpen; }

void WmVolumePopupClose()
{
    if (g_taskbarPopupPin.Is(TaskbarPopup::Volume)) return;
    g_volumePopupOpen = false;
    g_volumeDragging = false;
}

void WmVolumePopupGetRect(uint32_t screenW, uint32_t screenH,
                          int32_t* outX, int32_t* outY, uint32_t* outW, uint32_t* outH)
{
    const DesktopRect p = WmVolumePopupLayout(screenW, screenH).panel;
    *outX = p.x; *outY = p.y;
    *outW = p.w > 0 ? static_cast<uint32_t>(p.w) : 0;
    *outH = p.h > 0 ? static_cast<uint32_t>(p.h) : 0;
}

bool WmVolumeLeftPress(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (WmTaskbarHidden() || (WmLauncherVisible() && WmTaskbarPopupPinned())) return false;
    if (!g_volumePopupOpen) {
        if (!WmVolumeIconContains(mx, my, screenW, screenH)) return false;
        if (WmTaskbarPopupPinned()) return true;
        WmLauncherClose();
        WmTrayFlyoutClose();
        g_volumePopupOpen = true;
        PointerActivationCancel();
        return true;
    }
    const TaskbarVolumePopupLayout l = WmVolumePopupLayout(screenW, screenH);
    switch (TaskbarVolumePopupHit(WmDesktopMetrics(), l, mx, my)) {
    case TaskbarVolumeHit::Mute: WmVolumeApply(TaskbarVolumeToggleMute(g_volumeControl)); break;
    case TaskbarVolumeHit::Slider:
        g_volumeDragging = true;
        WmVolumeApply(TaskbarVolumeSliderValue(l.track, mx));
        break;
    case TaskbarVolumeHit::Pin: g_taskbarPopupPin.Toggle(TaskbarPopup::Volume); break;
    case TaskbarVolumeHit::Panel: break;
    case TaskbarVolumeHit::Outside:
        if (g_taskbarPopupPin.Is(TaskbarPopup::Volume)) return WmVolumeIconContains(mx, my, screenW, screenH);
        WmVolumePopupClose();
        break;
    }
    return true;
}

bool WmVolumeDragActive() { return g_volumeDragging; }

void WmVolumeDrag(int32_t mx, bool held, uint32_t screenW, uint32_t screenH)
{
    if (!g_volumeDragging) return;
    if (!held || !g_volumePopupOpen) { g_volumeDragging = false; return; }
    WmVolumeApply(TaskbarVolumeSliderValue(WmVolumePopupLayout(screenW, screenH).track, mx));
}

bool WmVolumeScroll(int32_t mx, int32_t my, int delta, uint32_t screenW, uint32_t screenH)
{
    if (!delta || WmTaskbarHidden()) return false;
    const auto popup = WmVolumePopupLayout(screenW, screenH);
    if (g_volumePopupOpen && popup.pin.contains(mx, my)) return false;
    const bool overPopup = g_volumePopupOpen && popup.panel.contains(mx, my);
    if (!overPopup && !WmVolumeIconContains(mx, my, screenW, screenH)) return false;
    WmVolumeApply(TaskbarVolumeWheel(g_volumeControl, delta));
    return true;
}

bool WmVolumeToggleMuteAt(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (WmTaskbarHidden()) return false;
    const auto popup = WmVolumePopupLayout(screenW, screenH);
    if (g_volumePopupOpen && popup.pin.contains(mx, my)) return false;
    const bool overPopup = g_volumePopupOpen && popup.panel.contains(mx, my);
    if (!overPopup && !WmVolumeIconContains(mx, my, screenW, screenH)) return false;
    WmVolumeApply(TaskbarVolumeToggleMute(g_volumeControl));
    return true;
}

static void WmCaptureVolumePopup(uint32_t screenW, uint32_t screenH, int32_t mouseX, int32_t mouseY,
                                 ChromeShellModel& snapshot)
{
    if (!g_volumePopupOpen) return;
    snapshot.kind = ChromeShellKind::VolumePopup;
    snapshot.volume.layout = WmVolumePopupLayout(screenW, screenH);
    snapshot.volume.control = g_volumeControl;
    snapshot.volume.available = g_volumeAvailable;
    snapshot.volume.dragging = g_volumeDragging;
    snapshot.volume.pinned = g_taskbarPopupPin.Is(TaskbarPopup::Volume);
    const auto& layout = snapshot.volume.layout;
    const auto hover = layout.pin.contains(mouseX, mouseY) ? layout.pin :
                       layout.mute.contains(mouseX, mouseY) ? layout.mute : DesktopRect{};
    snapshot.volume.mouseX = hover.w ? hover.x : -1;
    snapshot.volume.mouseY = hover.h ? hover.y : -1;
}

enum class WmTrayFlyout : uint8_t { None, Network, Temperature, Disk, Cpu, Memory, Compositor, App, Calendar };
static WmTrayFlyout g_trayFlyout;
static uint32_t g_diskPopupFirst;
static int32_t g_diskScrollRemainder;

bool WmDiskFlyoutSelected() { return g_trayFlyout == WmTrayFlyout::Disk; }

static ChromeDiskPopupLayout WmDiskPopupLayout(uint32_t sw, uint32_t sh)
{
    const auto bar = GetTaskbarLayout(sw, sh);
    DesktopRect anchor = bar.metrics[2];
    if (!anchor.w) {
        if (!g_taskbarPopupPin.Is(TaskbarPopup::Disk)) return {};
        // A resize or a hidden tray icon must not leave an inaccessible pin.
        anchor = {int(sw) - int(WmMetric(16)), bar.y, int(WmMetric(16)), 1};
    }
    return ChromeDiskPopupLayoutFor(WmChromeStyle(), anchor, int(sw), bar.y, DesktopDiskVolumeCount());
}

static void WmStepDiskPopup(const ChromeDiskPopupLayout& layout, int64_t step)
{
    g_diskPopupFirst = DesktopDiskPageStep(g_diskPopupFirst, DesktopDiskVolumeCount(), layout.pageRows, step);
}

bool WmDiskFlyoutScroll(int32_t x, int32_t y, int delta120, uint32_t sw, uint32_t sh)
{
    if (!WmDiskFlyoutSelected() || WmTaskbarHidden() || WmLauncherVisible() || WmWindowMenuOpen()) return false;
    const auto layout = WmDiskPopupLayout(sw, sh);
    if (!layout.panel.contains(x, y)) return false;
    const int64_t movement = g_diskScrollRemainder + int64_t(delta120);
    g_diskScrollRemainder = int32_t(movement % 120);
    WmStepDiskPopup(layout, -(movement / 120));
    return true;
}

DesktopStatsPanel WmDetailedStatsSelected()
{
    switch (g_trayFlyout) {
    case WmTrayFlyout::Cpu: return DesktopStatsPanel::Cpu;
    case WmTrayFlyout::Memory: return DesktopStatsPanel::Memory;
    case WmTrayFlyout::Compositor: return DesktopStatsPanel::Compositor;
    case WmTrayFlyout::App: return DesktopStatsPanel::App;
    default: return DesktopStatsPanel::None;
    }
}

static TaskbarPopup WmTrayPopupKind()
{
    switch (g_trayFlyout) {
    case WmTrayFlyout::Cpu: return TaskbarPopup::Cpu;
    case WmTrayFlyout::Memory: return TaskbarPopup::Memory;
    case WmTrayFlyout::Compositor: return TaskbarPopup::Compositor;
    case WmTrayFlyout::App: return TaskbarPopup::App;
    case WmTrayFlyout::Calendar: return TaskbarPopup::Calendar;
    default: break;
    }
    if (g_trayFlyout == WmTrayFlyout::Network) return TaskbarPopup::Network;
    if (WmDiskFlyoutSelected()) return TaskbarPopup::Disk;
    return g_trayFlyout == WmTrayFlyout::Temperature ? TaskbarPopup::Temperature : TaskbarPopup::None;
}

bool WmTrayFlyoutVisible() { return g_trayFlyout != WmTrayFlyout::None; }

void WmTrayFlyoutClose()
{
    if (!g_taskbarPopupPin.Is(WmTrayPopupKind())) g_trayFlyout = WmTrayFlyout::None;
}

bool WmTaskbarSystemPopupModal()
{
    return (g_volumePopupOpen && !g_taskbarPopupPin.Is(TaskbarPopup::Volume)) ||
           (WmTrayFlyoutVisible() && !g_taskbarPopupPin.Is(WmTrayPopupKind()));
}

bool WmTaskbarPopupDismissible()
{
    if (WmLauncherVisible() || WmWindowMenuOpen()) return false;
    const uint32_t owner = WmVisibleTrayMenuOwner();
    return WmTaskbarSystemPopupModal() ||
           (owner && !g_taskbarPopupPin.Is(TaskbarPopup::AppMenu, owner) && !WmTaskbarHidden());
}

void WmTaskbarPopupDismiss()
{
    WmVolumePopupClose();
    WmTrayFlyoutClose();
    const uint32_t owner = WmVisibleTrayMenuOwner();
    if (owner && !g_taskbarPopupPin.Is(TaskbarPopup::AppMenu, owner))
        ShellServiceQueueTrayInput({TrayInputKind::DismissMenu, owner, 0, 0, 0});
}

void WmCyclePerformanceHud()
{
    if (WmTaskbarPopupPinned()) return;
    g_hudMode = g_hudMode == WmHudMode::Off ? WmHudMode::Compact :
                g_hudMode == WmHudMode::Compact ? WmHudMode::Panel : WmHudMode::Off;
}

static uint32_t g_cpuFirstRow;
static int32_t g_cpuScrollRemainder;

static ChromeStatsPopupLayout WmStatsPopupLayout(uint32_t sw, uint32_t sh, DesktopStatsPanel panel)
{
    const auto bar = GetTaskbarLayout(sw, sh);
    unsigned metric;
    switch (panel) {
    case DesktopStatsPanel::Cpu: metric = 0; break;
    case DesktopStatsPanel::Memory: metric = 1; break;
    case DesktopStatsPanel::Compositor: metric = 4; break;
    case DesktopStatsPanel::App: metric = 5; break;
    default: return {};
    }
    auto anchor = bar.metrics[metric];
    const bool pinned = g_taskbarPopupPin.Is(WmTrayPopupKind());
    if (!anchor.w && pinned) anchor = {int(sw) - int(WmMetric(16)), bar.y, int(WmMetric(16)), 1};
    return ChromeStatsPopupLayoutFor(WmChromeStyle(), anchor, int(sw), bar.y, panel, DesktopStatsCoreCount(), pinned);
}

static void WmStepCpuPopup(const ChromeStatsPopupLayout& layout, int64_t step)
{
    g_cpuFirstRow = DesktopDiskPageStep(g_cpuFirstRow, (DesktopStatsCoreCount() + 1) / 2, layout.coreRows, step);
}

bool WmCpuFlyoutScroll(int32_t x, int32_t y, int delta120, uint32_t sw, uint32_t sh)
{
    if (WmDetailedStatsSelected() != DesktopStatsPanel::Cpu ||
        WmTaskbarHidden() || WmLauncherVisible() || WmWindowMenuOpen()) return false;
    const auto layout = WmStatsPopupLayout(sw, sh, DesktopStatsPanel::Cpu);
    if (!layout.coreRows || !layout.panel.contains(x, y)) return false;
    const int64_t movement = g_cpuScrollRemainder + int64_t(delta120);
    g_cpuScrollRemainder = int32_t(movement % 120);
    WmStepCpuPopup(layout, -(movement / 120));
    return true;
}

// An empty month follows the wall clock, including a local midnight rollover.
static CalendarMonth g_calendarMonth;

static ChromeCalendarLayout WmCalendarPopupLayout(uint32_t sw, uint32_t sh, CalendarDate today)
{
    const auto bar = GetTaskbarLayout(sw, sh);
    auto anchor = bar.clock;
    const bool pinned = g_taskbarPopupPin.Is(TaskbarPopup::Calendar);
    if (!anchor.w && pinned) anchor = {int(sw) - int(WmMetric(16)), bar.y, int(WmMetric(16)), 1};
    return ChromeCalendarLayoutFor(WmChromeStyle(), anchor, int(sw), bar.y, CalendarDateValid(today), pinned);
}

static void WmCalendarNavigate(int32_t x, int32_t y, uint32_t sw, uint32_t sh)
{
    const auto today = RtcReadTaskbarClock().date;
    const auto layout = WmCalendarPopupLayout(sw, sh, today);
    const int direction = layout.previous.contains(x, y) ? -1 : layout.next.contains(x, y) ? 1 : 0;
    if (!direction) return;
    const auto selected = CalendarMonthValid(g_calendarMonth) ? g_calendarMonth : today.month;
    const auto target = CalendarStepMonth(selected, direction);
    g_calendarMonth = target == today.month ? CalendarMonth{} : target;
}

static DesktopRect WmTrayFlyoutLayout(uint32_t sw, uint32_t sh);

bool WmTrayFlyoutPress(int32_t mx, int32_t my, uint32_t sw, uint32_t sh)
{
    if (WmTaskbarHidden() || (WmLauncherVisible() && WmTaskbarPopupPinned())) return false;
    const auto panel = WmTrayFlyoutLayout(sw, sh);
    if (panel.contains(mx, my)) {
        if (ChromeTrayFlyoutPinRect(WmChromeStyle(), panel).contains(mx, my))
            g_taskbarPopupPin.Toggle(WmTrayPopupKind());
        else if (WmDiskFlyoutSelected()) {
            const auto layout = WmDiskPopupLayout(sw, sh);
            if (layout.previous.contains(mx, my)) WmStepDiskPopup(layout, -int64_t(layout.pageRows));
            if (layout.next.contains(mx, my)) WmStepDiskPopup(layout, layout.pageRows);
        } else if (WmDetailedStatsSelected() == DesktopStatsPanel::Cpu) {
            const auto layout = WmStatsPopupLayout(sw, sh, DesktopStatsPanel::Cpu);
            if (layout.previous.contains(mx, my)) WmStepCpuPopup(layout, -int64_t(layout.coreRows));
            if (layout.next.contains(mx, my)) WmStepCpuPopup(layout, layout.coreRows);
        } else if (g_trayFlyout == WmTrayFlyout::Calendar) {
            WmCalendarNavigate(mx, my, sw, sh);
        }
        return true;
    }
    const auto l = GetTaskbarLayout(sw, sh);
    if (l.hud.contains(mx, my)) {
        if (WmTaskbarPopupPinned()) return true;
        g_hudMode = WmHudMode::Panel;
        WmTrayFlyoutClose();
        return true;
    }
    const auto next = l.network.contains(mx, my) ? WmTrayFlyout::Network :
                      l.metrics[6].contains(mx, my) ? WmTrayFlyout::Temperature :
                      l.metrics[2].contains(mx, my) ? WmTrayFlyout::Disk :
                      l.metrics[0].contains(mx, my) ? WmTrayFlyout::Cpu :
                      l.metrics[1].contains(mx, my) ? WmTrayFlyout::Memory :
                      l.metrics[4].contains(mx, my) ? WmTrayFlyout::Compositor :
                      l.metrics[5].contains(mx, my) ? WmTrayFlyout::App :
                      l.clock.contains(mx, my) ? WmTrayFlyout::Calendar : WmTrayFlyout::None;
    if (WmTaskbarPopupPinned()) return next != WmTrayFlyout::None;
    if (!WmTrayFlyoutVisible() && next == WmTrayFlyout::None) return false;
    g_trayFlyout = next == g_trayFlyout ? WmTrayFlyout::None : next;
    if (WmTrayFlyoutVisible()) {
        if (g_trayFlyout == WmTrayFlyout::Calendar) g_calendarMonth = {};
        if (WmDiskFlyoutSelected()) g_diskPopupFirst = g_diskScrollRemainder = 0;
        if (WmDetailedStatsSelected() == DesktopStatsPanel::Cpu) g_cpuFirstRow = g_cpuScrollRemainder = 0;
        if (WmTrayFlyoutLayout(sw, sh).w <= 0) {
            WmTrayFlyoutClose();
            return true;
        }
        WmLauncherClose();
        WmVolumePopupClose();
        PointerActivationCancel();
    }
    return true;
}

// Whether the open flyout has live content: the network has an address, or a
// package temperature is available. It sizes the panel to its content.
static bool WmTrayFlyoutLive()
{
    if (g_trayFlyout == WmTrayFlyout::Network) return __atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE) != 0;
    ThermalSnapshot snap{};
    return ThermalReadSnapshot(snap) && snap.latest.pkgC >= 0;
}

static DesktopRect WmTrayFlyoutLayout(uint32_t sw, uint32_t sh)
{
    if (!WmTrayFlyoutVisible() || WmTaskbarHidden()) return {};
    if (WmDiskFlyoutSelected()) return WmDiskPopupLayout(sw, sh).panel;
    if (g_trayFlyout == WmTrayFlyout::Calendar) return WmCalendarPopupLayout(sw, sh, RtcReadTaskbarClock().date).panel;
    const auto stats = WmDetailedStatsSelected();
    if (stats != DesktopStatsPanel::None) return WmStatsPopupLayout(sw, sh, stats).panel;
    const auto l = GetTaskbarLayout(sw, sh);
    const auto anchor = g_trayFlyout == WmTrayFlyout::Network ? l.network : l.metrics[6];
    const int gap = WmMetric(ChromeShellMetrics::PanelMargin);
    const int h = ChromeTrayFlyoutHeight(WmChromeStyle(), g_trayFlyout == WmTrayFlyout::Network, WmTrayFlyoutLive());
    if (!anchor.w || l.y < h + 2 * gap || sw < WmMetric(180)) return {};
    // 304 wide, centred on its tray target and kept 8 px from the screen
    // edge and the taskbar (as the prototype's fly()).
    const int available = int(sw) - 2 * gap;
    const int preferred = WmMetric(304);
    return TaskbarTooltipBox(anchor, preferred < available ? preferred : available, h, l.y, gap, sw, gap);
}

static bool WmHudThermal()
{
    ThermalSnapshot snap{};
    return ThermalReadSnapshot(snap) && snap.latest.pkgC >= 0;
}

static DesktopRect WmPerformanceHudLayout(uint32_t sw, uint32_t sh)
{
    if (WmTaskbarHidden() || g_hudMode != WmHudMode::Panel) return {};
    const int margin = WmMetric(ChromeShellMetrics::PanelMargin);
    const int deskH = WmDesktopHeight(sh);
    const int h = ChromePerformanceHudHeight(WmChromeStyle(), WmHudThermal());
    const int w = WmMetric(348);
    if (int(sw) < w + 2 * margin || deskH < h + 2 * margin) return {};
    DesktopRect panel{int(sw) - w - margin, deskH - h - margin, w, h};
    int32_t dx = 0, dy = 0;
    uint32_t dw = 0, dh = 0;
    bool material = false;
    if (WmDockSurfaceRect(sw, sh, &dx, &dy, &dw, &dh, &material) &&
        panel.x < dx + int(dw) && dx < panel.x + panel.w &&
        panel.y < dy + int(dh) && dy < panel.y + panel.h) {
        if (dx > int(sw / 2)) panel.x = dx - margin - w;
        else panel.x = dx + int(dw) + margin;
        if (panel.x < margin || panel.x + w > int(sw) - margin) return {};
    }
    return panel;
}

static bool WmPanelRect(DesktopRect r, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h)
{
    if (r.w <= 0 || r.h <= 0) return false;
    *x = r.x; *y = r.y; *w = r.w; *h = r.h;
    return true;
}

bool WmTrayFlyoutRect(uint32_t sw, uint32_t sh, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h)
{
    return WmPanelRect(WmTrayFlyoutLayout(sw, sh), x, y, w, h);
}

static DesktopRect WmInteractiveHudLayout(uint32_t sw, uint32_t sh)
{
    // This optional panel yields to the shared popup slot. Once pinned no
    // competing popup can replace it.
    if (WmLauncherVisible() || g_volumePopupOpen || WmTrayFlyoutVisible() || WmWindowMenuOpen() ||
        WmVisibleTrayMenuOwner()) return {};
    return WmPerformanceHudLayout(sw, sh);
}

bool WmPerformanceHudRect(uint32_t sw, uint32_t sh, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h)
{
    return WmPanelRect(WmInteractiveHudLayout(sw, sh), x, y, w, h);
}

bool WmTaskbarPopupPointerOwned(int32_t x, int32_t y, uint32_t sw, uint32_t sh)
{
    if (WmTaskbarHidden() || WmLauncherVisible() || WmWindowMenuOpen()) return false;
    return (g_volumePopupOpen && TaskbarPopupOwnsPointer(WmVolumePopupLayout(sw, sh).panel,
                g_taskbarPopupPin.Is(TaskbarPopup::Volume), x, y)) ||
           TaskbarPopupOwnsPointer(WmTrayFlyoutLayout(sw, sh),
                g_taskbarPopupPin.Is(WmTrayPopupKind()), x, y) ||
           WmInteractiveHudLayout(sw, sh).contains(x, y);
}

bool WmPerformanceHudPress(int32_t x, int32_t y, uint32_t sw, uint32_t sh)
{
    const auto panel = WmInteractiveHudLayout(sw, sh);
    if (!panel.contains(x, y)) return false;
    if (ChromePerformanceHudPinRect(WmChromeStyle(), panel).contains(x, y))
        g_taskbarPopupPin.Toggle(TaskbarPopup::Performance);
    return true;
}

static ChromePerformanceHudModel WmHudModel(DesktopRect rect)
{
    // Snapshot the histories together. Polling keys and rasterising the
    // result use the same data even when a client reports a new frame.
    static ChromePerformanceHudModel model{};
    static DesktopIntervals<> app, comp;
    static ThermalSample temp[ThermalGraphHistory];
    const uint64_t now = ApicTickCount();
    if (!model.telemetry.app || now - model.tick >= 500) {
        model.tick = now;
        model.telemetry = WmTelemetryModel(true, true);
        app = *model.telemetry.app;
        comp = *model.telemetry.compositor;
        model.telemetry.app = &app;
        model.telemetry.compositor = &comp;
        for (unsigned i = 0; i < model.telemetry.pointCount; ++i) temp[i] = model.telemetry.points[i];
        model.telemetry.points = temp;
    }
    model.rect = rect;
    return model;
}

static void WmCapturePopupPin(DesktopRect pin, int32_t mx, int32_t my, bool pinned, ChromeShellModel& snapshot)
{
    const bool hovered = pin.contains(mx, my);
    snapshot.mouseX = hovered ? pin.x : -1;
    snapshot.mouseY = hovered ? pin.y : -1;
    snapshot.popupPinned = pinned;
}

static void WmCaptureTrayFlyout(const WmShellSurface& surface, uint32_t sw, uint32_t sh,
                               uint64_t uptimeMs, int32_t mx, int32_t my, ChromeShellModel& snapshot)
{
    if (g_trayFlyout == WmTrayFlyout::Calendar) {
        const auto today = RtcReadTaskbarClock().date;
        const auto layout = WmCalendarPopupLayout(sw, sh, today);
        const auto rect = layout.panel;
        if (rect.w <= 0 || rect.x != surface.x || rect.y != surface.y ||
            uint32_t(rect.w) != surface.w || uint32_t(rect.h) != surface.h) return;
        snapshot.kind = ChromeShellKind::TrayFlyout;
        snapshot.rect = rect;
        snapshot.calendar = true;
        snapshot.calendarLayout = layout;
        snapshot.calendarToday = today;
        snapshot.calendarMonth = CalendarMonthValid(g_calendarMonth) ? g_calendarMonth : today.month;
        const auto hover = layout.pin.contains(mx, my) ? layout.pin :
                           layout.previous.contains(mx, my) ? layout.previous :
                           layout.next.contains(mx, my) ? layout.next : DesktopRect{};
        WmCapturePopupPin(hover, mx, my, g_taskbarPopupPin.Is(TaskbarPopup::Calendar), snapshot);
        return;
    }
    const auto panel = WmDetailedStatsSelected();
    if (panel != DesktopStatsPanel::None) {
        const auto layout = WmStatsPopupLayout(sw, sh, panel);
        const auto rect = layout.panel;
        if (rect.w <= 0 || rect.x != surface.x || rect.y != surface.y ||
            uint32_t(rect.w) != surface.w || uint32_t(rect.h) != surface.h) return;
        snapshot.kind = ChromeShellKind::TrayFlyout;
        snapshot.rect = rect;
        snapshot.statsLayout = layout;
        snapshot.statsPanel = panel;
        const auto hover = layout.pin.contains(mx, my) ? layout.pin :
                           layout.previous.contains(mx, my) ? layout.previous :
                           layout.next.contains(mx, my) ? layout.next : DesktopRect{};
        WmCapturePopupPin(hover, mx, my, g_taskbarPopupPin.Is(WmTrayPopupKind()), snapshot);
        if (panel == DesktopStatsPanel::Cpu || panel == DesktopStatsPanel::Memory) {
            DesktopStatsRead(panel, snapshot.detailedStats);
            if (panel == DesktopStatsPanel::Cpu) {
                const auto cores = DesktopStatsCoreCount();
                auto& stats = snapshot.detailedStats;
                for (unsigned i = stats.coreCount; i < cores; ++i) stats.cores[i] = DetailedNoPercent;
                stats.coreCount = cores;
                snapshot.firstCoreRow = DesktopDiskPageFirst(g_cpuFirstRow, (cores + 1) / 2, layout.coreRows);
                g_cpuFirstRow = snapshot.firstCoreRow;
            }
        } else {
            snapshot.hudTick = ShellCadenceTick;
            const bool app = panel == DesktopStatsPanel::App;
            const auto source = WmTelemetryModel(app, false, false);
            auto& telemetry = snapshot.telemetry;
            telemetry.hasApp = app;
            telemetry.hasCompositor = !app;
            if (app) WmCaptureCadence(telemetry.app, *source.app, uptimeMs);
            else WmCaptureCadence(telemetry.compositor, *source.compositor, uptimeMs);
        }
        return;
    }
    if (WmDiskFlyoutSelected()) {
        const auto layout = WmDiskPopupLayout(sw, sh);
        // The worker can adopt a new volume count between choosing the
        // surface and building its model. Let the next frame use its geometry.
        if (!layout.pageRows || layout.panel.x != surface.x || layout.panel.y != surface.y ||
            uint32_t(layout.panel.w) != surface.w || uint32_t(layout.panel.h) != surface.h) return;
        snapshot.kind = ChromeShellKind::TrayFlyout;
        snapshot.rect = layout.panel;
        snapshot.diskLayout = layout;
        snapshot.diskPopup = true;
        g_diskPopupFirst = DesktopDiskReadPage(g_diskPopupFirst, layout.pageRows, snapshot.disk);
        snapshot.popupPinned = g_taskbarPopupPin.Is(TaskbarPopup::Disk);
        const auto hover = layout.pin.contains(mx, my) ? layout.pin :
                           layout.previous.contains(mx, my) ? layout.previous :
                           layout.next.contains(mx, my) ? layout.next : DesktopRect{};
        snapshot.mouseX = hover.w ? hover.x : -1;
        snapshot.mouseY = hover.h ? hover.y : -1;
        const auto stats = WmDesktopTelemetryRead();
        snapshot.telemetry.stats.readBytesPerSecond = WmShellRate(stats.readBytesPerSecond);
        snapshot.telemetry.stats.writeBytesPerSecond = WmShellRate(stats.writeBytesPerSecond);
        return;
    }
    const auto rect = WmTrayFlyoutLayout(sw, sh);
    if (rect.w <= 0) return;
    const bool network = g_trayFlyout == WmTrayFlyout::Network;
    const uint32_t address = __atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE);
    if (network && address) {
        BootText label(snapshot.text, sizeof(snapshot.text));
        const auto* bytes = reinterpret_cast<const uint8_t*>(&address);
        for (unsigned i = 0; i < 4; ++i) {
            if (i) label.Add(".");
            label.Number(bytes[i]);
        }
    }
    snapshot.kind = ChromeShellKind::TrayFlyout;
    snapshot.rect = rect;
    WmCapturePopupPin(ChromeTrayFlyoutPinRect(WmChromeStyle(), rect), mx, my,
                      g_taskbarPopupPin.Is(WmTrayPopupKind()), snapshot);
    snapshot.network = network;
    snapshot.networkConnected = address != 0;
    const auto source = WmTelemetryModel(false, !network);
    if (!network) WmCaptureThermal(snapshot.telemetry, source, true);
    else if (snapshot.networkConnected) {
        snapshot.telemetry.stats.downBytesPerSecond = WmShellRate(source.stats.downBytesPerSecond);
        snapshot.telemetry.stats.upBytesPerSecond = WmShellRate(source.stats.upBytesPerSecond);
    }
}

static void WmCapturePerformanceHud(uint32_t sw, uint32_t sh, int32_t mx, int32_t my, ChromeShellModel& snapshot)
{
    const auto rect = WmPerformanceHudLayout(sw, sh);
    if (rect.w <= 0) return;
    const auto model = WmHudModel(rect);
    snapshot.kind = ChromeShellKind::PerformanceHud;
    snapshot.rect = model.rect;
    WmCapturePopupPin(ChromePerformanceHudPinRect(WmChromeStyle(), rect), mx, my,
                      g_taskbarPopupPin.Is(TaskbarPopup::Performance), snapshot);
    snapshot.hudTick = ShellCadenceTick;
    WmCaptureHudCadence(snapshot.telemetry, model);
    WmCaptureThermal(snapshot.telemetry, model.telemetry, true);
}

static void WmCaptureTaskbarInitial(ChromeShellEntry& entry, const LauncherItem* item, const char* title)
{
    int app = -1;
    if (item) {
        char key[LauncherLimits::KeyLength];
        LauncherItemKey(*item, key);
        app = g_launcherModel.FindApp(key);
    }
    const char* name = app >= 0 ? g_launcherModel.Name(uint32_t(app)) : item ? item->title : title;
    entry.initial = *name ? *name : '?';
    entry.color = app >= 0 ? g_launcherModel.IconColor(uint32_t(app)) : LauncherColor::Slate;
}

static void WmCaptureTaskbar(uint32_t screenW, uint32_t screenH, uint64_t uptimeMs,
                             int32_t mouseX, int32_t mouseY, ChromeShellModel& snapshot)
{
    if (!g_wmActive) return;
    LauncherReadGuard icons;
    snapshot.kind = ChromeShellKind::Taskbar;
    const auto& tray = ShellServiceTrayState();
    snapshot.tray.itemCount = tray.itemCount;
    for (uint32_t i = 0; i < tray.itemCount; ++i) snapshot.tray.items[i] = tray.items[i];
    snapshot.taskbar = GetTaskbarLayout(screenW, screenH);
    auto& layout = snapshot.taskbar;
    bool busy = false;
    for (int i = 0; i < layout.count; ++i) {
        const int index = layout.indices[i];
        layout.indices[i] = 0; // only the WM hit test needs these identities
        auto& entry = snapshot.entries[i];
        auto& icon = snapshot.icons[i];
        if (index <= -10) {
            const auto slot = static_cast<unsigned>(-10 - index);
            SpinLockAcquire(&g_launchLock);
            entry.busy = g_launches.entries[slot].busy();
            if (g_launchRequests[slot].hasIcon) WmCaptureShellIcon(icon, g_launchRequests[slot].icon);
            else {
                const auto& launch = g_launches.entries[slot];
                const LauncherItem* matched = nullptr;
                for (uint32_t item = 0; item < icons.get().count && !matched; ++item)
                    if (LaunchFeedbackTable::Exact(icons.get().items[item].catalogId, launch.identity)) matched = &icons.get().items[item];
                WmCaptureTaskbarInitial(entry, matched, launch.title);
            }
            SpinLockRelease(&g_launchLock);
            entry.launch = true;
            busy |= entry.busy;
            continue;
        }
        const Window& window = g_windows[index];
        entry.active = window.focused && !window.minimized;
        entry.minimized = window.minimized;
        if (layout.buttons[i].w >= int(WmMetric(ChromeShellMetrics::TaskbarLabelMin)))
            WmStrCopy(entry.label, WindowDisplayTitle(window), sizeof(entry.label));
        const LauncherItem* matched = nullptr;
        WmCaptureShellIcon(icon, WindowIcon(window, icons.get(), &matched));
        if (!icon.present) WmCaptureTaskbarInitial(entry, matched, WindowDisplayTitle(window));
    }
    layout.nextIndex = 0;
    snapshot.launcherOpen = g_launcherOpen;
    if (layout.network.w > 0)
        snapshot.networkConnected = __atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE) != 0;
    if (layout.volume.w > 0) {
        snapshot.volumeControl = g_volumeControl;
        snapshot.volumeAvailable = g_volumeAvailable;
    }
    if (layout.agent.w > 0) {
        const uint8_t agent = __atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE);
        snapshot.agentShown = agent != 0;
        snapshot.agentStale = agent == static_cast<uint8_t>(BridgePresenceState::Stale);
    }
    snapshot.uptimeMs = ShellCadenceTick + (busy ? (uptimeMs / 100 % 8) * 100 : 0);
    const DesktopRect bounds{0, layout.y, layout.screenW, layout.screenH - layout.y};
    const bool hovered = bounds.contains(mouseX, mouseY);
    snapshot.mouseX = hovered ? mouseX : -1;
    snapshot.mouseY = hovered ? mouseY : -1;
    WmCaptureShellIcon(snapshot.icons[ChromeShellModel::AppsIcon], icons.get().appsIcon);
    WmCaptureShellIcon(snapshot.icons[ChromeShellModel::TerminalIcon], icons.get().terminalIcon);
    const bool comp = layout.metrics[4].w != 0, app = layout.metrics[5].w != 0;
    const auto source = WmTelemetryModel(app, layout.metrics[6].w != 0, false);
    auto& telemetry = snapshot.telemetry;
    if (layout.metrics[0].w > 0) telemetry.stats.cpuPercent = source.stats.cpuPercent;
    if (layout.metrics[1].w > 0) telemetry.stats.memoryPercent = source.stats.memoryPercent;
    if (layout.metrics[3].w > 0) {
        telemetry.stats.downBytesPerSecond = WmShellRate(source.stats.downBytesPerSecond);
        telemetry.stats.upBytesPerSecond = WmShellRate(source.stats.upBytesPerSecond);
    }
    if (comp) {
        telemetry.hasCompositor = true;
        WmCaptureCadence(telemetry.compositor, *source.compositor, uptimeMs);
    }
    if (app) {
        telemetry.hasApp = true;
        WmCaptureCadence(telemetry.app, *source.app, uptimeMs, true);
    }
    WmCaptureThermal(telemetry, source, false);
    if (layout.hud.w > 0) {
        snapshot.hudTick = ShellCadenceTick;
        WmCaptureHudCadence(snapshot.compactTelemetry, WmHudModel(layout.hud));
    }
}

// Tooltips float above the taskbar, centred on the hovered control. The
// launcher, volume popup and tray flyout take precedence.
static bool WmTrayTooltip(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                          char (&text)[48], const char*& detail, DesktopRect& box)
{
    if (!g_wmActive || g_launcherOpen || g_volumePopupOpen || WmTrayFlyoutVisible() || WmTaskbarHidden() ||
        WmInteractiveHudLayout(screenW, screenH).w > 0) return false;
    const TaskbarLayout l = GetTaskbarLayout(screenW, screenH);
    const WmRect* anchor = nullptr;
    if (l.metrics[2].contains(mx, my)) {
        anchor = &l.metrics[2];
        WmStrCopy(text, "Disk", sizeof(text));
    } else if (l.volume.contains(mx, my)) {
        anchor = &l.volume;
        TaskbarVolumeTooltip(text, g_volumeControl, g_volumeAvailable);
    } else if (l.metrics[6].w && l.metrics[6].contains(mx, my)) {
        anchor = &l.metrics[6];
        DesktopThermalTooltip(text);
    } else if (l.agent.w && l.agent.contains(mx, my)) {
        anchor = &l.agent;
        AgentIndicatorTooltip(text);
    } else if (l.network.contains(mx, my)) {
        anchor = &l.network;
        const uint32_t address = __atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE);
        if (address) {
            const uint8_t* o = reinterpret_cast<const uint8_t*>(&address);
            WmStrCopy(text, "Network: ", sizeof(text));
            char* out = text + strlen(text);
            for (int i = 0; i < 4; ++i) {
                DesktopNumber(out, o[i]);
                while (*out) ++out;
                if (i < 3) *out++ = '.';
            }
            *out = 0;
        } else WmStrCopy(text, "Network: disconnected", sizeof(text));
    }
    if (!anchor) {
        constexpr unsigned metrics[] = {0, 1, 4, 5};
        const char* labels[] = {"CPU usage", "Memory usage", "Compositor frame timing", "App frame timing"};
        for (unsigned i = 0; i < 4; ++i) {
            if (!l.metrics[metrics[i]].contains(mx, my)) continue;
            anchor = &l.metrics[metrics[i]];
            WmStrCopy(text, labels[i], sizeof(text));
            break;
        }
    }
    if (!anchor) {
        const WmRect* controls[] = {&l.apps, &l.terminal, &l.next};
        const char* labels[] = {"Applications", "New terminal", "More windows"};
        for (unsigned i = 0; i < sizeof(controls) / sizeof(*controls); ++i) {
            if (!controls[i]->contains(mx, my)) continue;
            anchor = controls[i];
            WmStrCopy(text, labels[i], sizeof(text));
            break;
        }
    }
    if (!anchor) {
        for (int i = 0; i < l.count; ++i) {
            if (!l.buttons[i].contains(mx, my)) continue;
            const int index = l.indices[i];
            if (index <= -10) {
                SpinLockAcquire(&g_launchLock);
                const LaunchFeedback entry = g_launches.entries[-10 - index];
                SpinLockRelease(&g_launchLock);
                WmStrCopy(text, entry.busy() ? "Starting: " :
                    entry.phase == LaunchPhase::TimedOut ? "Still waiting: " :
                    entry.phase == LaunchPhase::Exited ? "Exited without a window: " : "Launch failed: ", sizeof(text));
                const size_t length = strlen(text);
                WmStrCopy(text + length, entry.title, sizeof(text) - length);
            } else WmStrCopy(text, WindowDisplayTitle(g_windows[index]), sizeof(text));
            if (!text[0]) break;
            anchor = &l.buttons[i];
            break;
        }
    }
    if (!anchor && l.build.contains(mx, my)) {
        anchor = &l.build;
        // Payload (or "Brook build"), then commit with any dirty suffix and the build date.
        static char buildDetail[64];
        WmStrCopy(text, g_buildPayloadId[0] ? g_buildPayloadId : "Brook build", sizeof(text));
        WmStrCopy(buildDetail, BuildGitHash(), sizeof(buildDetail));
        const size_t used = strlen(buildDetail);
        WmStrCopy(buildDetail + used, "  built ", sizeof(buildDetail) - used);
        WmStrCopy(buildDetail + strlen(buildDetail), BuildDate(), sizeof(buildDetail) - strlen(buildDetail));
        detail = buildDetail;
    }
    if (!anchor) return false;
    const ChromeStyle style = WmChromeStyle();
    const int margin = WmMetric(4);
    int width = ChromeTooltipWidth(style, text, detail);
    if (width > int(screenW) - 2 * margin) width = int(screenW) - 2 * margin;
    box = TaskbarTooltipBox(*anchor, width, ChromeTooltipHeight(style, detail != nullptr), l.y,
                            WmMetric(ChromeShellMetrics::PanelMargin), static_cast<int>(screenW), margin);
    return box.w > 0 && box.h > 0;
}

bool WmTrayTooltipGetRect(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                          int32_t* outX, int32_t* outY, uint32_t* outW, uint32_t* outH)
{
    char text[48]{};
    const char* detail = nullptr;
    DesktopRect box{};
    if (!WmTrayTooltip(screenW, screenH, mx, my, text, detail, box)) return false;
    *outX = box.x; *outY = box.y; *outW = static_cast<uint32_t>(box.w); *outH = static_cast<uint32_t>(box.h);
    return true;
}

static void WmCaptureTooltip(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                             ChromeShellModel& snapshot)
{
    char text[48]{};
    const char* detail = nullptr;
    DesktopRect box{};
    if (!WmTrayTooltip(screenW, screenH, mx, my, text, detail, box)) return;
    snapshot.kind = ChromeShellKind::Tooltip;
    snapshot.rect = box;
    WmStrCopy(snapshot.text, text, sizeof(snapshot.text));
    if (detail) WmStrCopy(snapshot.detail, detail, sizeof(snapshot.detail));
}

int WmTaskbarHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (mx < 0 || my < 0 || mx >= static_cast<int32_t>(screenW) || my >= static_cast<int32_t>(screenH)) return -1;
    if (WmTaskbarHidden()) return -1;
    const TaskbarLayout l = GetTaskbarLayout(screenW, screenH);
    if (l.apps.contains(mx, my)) return -3;
    if (l.terminal.contains(mx, my)) return -2;
    if (l.next.contains(mx, my)) return l.nextIndex <= -10 ? l.nextIndex - 90 : l.nextIndex;
    for (int i = 0; i < l.count; ++i)
        if (l.buttons[i].contains(mx, my)) return l.indices[i];
    return -1;
}


static int ShellTrayAt(const ChromeTaskbarLayout& bar, int32_t x, int32_t y)
{
    for (uint32_t i = 0; i < bar.trayAppCount; ++i)
        if (bar.trayApps[i].contains(x, y)) return static_cast<int>(i);
    return -1;
}

void WmGetShellTrayGeometry(uint32_t sw, uint32_t sh, int32_t x, int32_t y, WmShellTrayGeometry& out)
{
    out = {};
    const auto& tray = ShellServiceTrayState();
    const uint32_t owner = WmVisibleTrayMenuOwner();
    g_taskbarPopupPin.RetainAppMenu(owner);
    const auto style = WmChromeStyle();
    const auto hud = WmInteractiveHudLayout(sw, sh);
    if (tray.notice.id) {
        const auto notice = ChromeNoticeRect(style, sw, WmDesktopHeight(sh), tray.notice.actionCount);
        if (hud.w <= 0 || notice.x >= hud.x + hud.w || hud.x >= notice.x + notice.w ||
            notice.y >= hud.y + hud.h || hud.y >= notice.y + notice.h)
            out.notice = notice;
    }
    // The window menu borrows the tray menu's surface (wm_window_menu.inc).
    if (g_windowMenu.open) {
        out.popup = WindowMenuRectLocked(sw, sh);
        out.menu = out.windowMenu = out.popup.w > 0;
        return;
    }
    if (!tray.itemCount || WmTaskbarHidden() || WmLauncherVisible() ||
        WmVolumePopupVisible() || WmTrayFlyoutVisible()) return;
    if (y < static_cast<int32_t>(WmTaskbarTop(sh)) && !owner) return;
    const auto bar = GetTaskbarLayout(sw, sh);
    if (owner) {
        for (uint32_t i = 0; i < bar.trayAppCount; ++i) {
            if (tray.items[i].id != owner) continue;
            out.popup = ChromeTrayMenuRect(style, bar.trayApps[i], tray.menuCount, sw);
            out.menu = out.popup.w > 0;
            return;
        }
        return;
    }
    if (hud.w > 0) return;
    const int hover = ShellTrayAt(bar, x, y);
    if (hover < 0) return;
    const auto& item = tray.items[hover];
    const auto anchor = bar.trayApps[hover];
    const int widthLimit = static_cast<int>(sw) - int(style.M(16));
    if (widthLimit <= 0) return;
    int width = style.TextWidth(item.title, ChromeFont::Label);
    const int detail = style.TextWidth(item.tooltip, ChromeFont::Label);
    if (detail > width) width = detail;
    width += 2 * ChromeTooltipPadding(style);
    if (width > widthLimit) width = widthLimit;
    const int height = style.Line(ChromeFont::Label) * (item.tooltip[0] ? 2 : 1) + 2 * style.M(8);
    int start = anchor.x + anchor.w - width;
    if (start < int(style.M(8))) start = style.M(8);
    if (anchor.y >= height + int(style.M(8)))
        out.popup = {start, anchor.y - height - int(style.M(8)), width, height};
}

bool WmShellTrayPointer(uint32_t sw, uint32_t sh, int32_t x, int32_t y,
                       bool pressed, int button, int scroll)
{
    if (WindowMenuPointer(sw, sh, x, y, pressed, button)) return true;
    const auto& tray = ShellServiceTrayState();
    if (!tray.itemCount && !tray.notice.id) return false;
    WmShellTrayGeometry geometry{};
    WmGetShellTrayGeometry(sw, sh, x, y, geometry);
    const auto style = WmChromeStyle();
    auto send = [&](TrayInputKind kind, uint32_t id, int32_t argument) {
        ShellServiceQueueTrayInput({kind, id, argument, x, y});
    };
    const bool pinned = g_taskbarPopupPin.Is(TaskbarPopup::AppMenu, tray.menuOwner);
    if (geometry.menu && TaskbarPopupOwnsPointer(geometry.popup, pinned, x, y)) {
        if (pressed) {
            if (!geometry.popup.contains(x, y)) send(TrayInputKind::DismissMenu, tray.menuOwner, 0);
            else if (!button && ChromeTrayMenuPinRect(style, geometry.popup).contains(x, y))
                g_taskbarPopupPin.Toggle(TaskbarPopup::AppMenu, tray.menuOwner);
            else if (!button) for (uint32_t i = 0; i < tray.menuCount; ++i) {
                const auto& row = tray.menu[i];
                if (row.enabled && !row.separator && ChromeTrayMenuRowRect(style, geometry.popup, i, true).contains(x, y))
                    send(pinned ? TrayInputKind::MenuSelectPinned : TrayInputKind::MenuSelect, tray.menuOwner, row.id);
            }
        }
        return true;
    }
    if (geometry.notice.contains(x, y) && !WmLauncherVisible() && !WmVolumePopupVisible() && !WmTrayFlyoutVisible()) {
        if (pressed && !button) {
            if (ChromeNoticeCloseRect(style, geometry.notice).contains(x, y))
                send(TrayInputKind::CloseNotice, tray.notice.id, 0);
            for (uint32_t i = 0; i < tray.notice.actionCount; ++i)
                if (ChromeNoticeActionRect(style, geometry.notice, tray.notice.actionCount, i).contains(x, y))
                    send(TrayInputKind::NoticeAction, tray.notice.id, i);
        }
        return true;
    }
    if (!tray.itemCount || WmTaskbarHidden() || WmLauncherVisible() ||
        WmTaskbarSystemPopupModal() || y < static_cast<int32_t>(WmTaskbarTop(sh))) return false;
    const int hover = ShellTrayAt(GetTaskbarLayout(sw, sh), x, y);
    if (hover < 0) return false;
    if (pressed && button <= 1) {
        const bool popupPinned = WmTaskbarPopupPinned();
        if (!button) send(popupPinned ? TrayInputKind::ActivateWithoutMenu : TrayInputKind::Activate,
                          tray.items[hover].id, 0);
        else if (!popupPinned) send(TrayInputKind::Context, tray.items[hover].id, 0);
    }
    if (scroll) send(TrayInputKind::Scroll, tray.items[hover].id, scroll);
    return true;
}

static void WmCaptureAppTray(const WmShellSurface& surface, uint32_t sw, uint32_t sh,
                            int32_t x, int32_t y, ChromeShellModel& model)
{
    const auto& tray = ShellServiceTrayState();
    model.rect = {surface.x, surface.y, int(surface.w), int(surface.h)};
    model.mouseX = x;
    model.mouseY = y;
    if (surface.kind == WmShellSurfaceKind::Notification) {
        model.kind = ChromeShellKind::Notification;
        model.tray.notice = tray.notice;
    } else if (surface.kind == WmShellSurfaceKind::AppTrayMenu) {
        model.kind = ChromeShellKind::AppTrayMenu;
        if (g_windowMenu.open) {
            model.tray.menuCount = g_windowMenu.count;
            for (uint32_t i = 0; i < g_windowMenu.count; ++i) model.tray.menu[i] = g_windowMenu.rows[i];
            return;
        }
        model.popupPinnable = true;
        model.popupPinned = g_taskbarPopupPin.Is(TaskbarPopup::AppMenu, tray.menuOwner);
        model.tray.menuCount = tray.menuCount;
        for (uint32_t i = 0; i < tray.menuCount; ++i) model.tray.menu[i] = tray.menu[i];
    } else {
        const int hover = ShellTrayAt(GetTaskbarLayout(sw, sh), x, y);
        if (hover < 0) return;
        model.kind = ChromeShellKind::Tooltip;
        WmStrCopy(model.text, tray.items[hover].title, sizeof(model.text));
        WmStrCopy(model.detail, tray.items[hover].tooltip, sizeof(model.detail));
    }
}

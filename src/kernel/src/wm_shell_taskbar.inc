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

// Debug-bridge agent indicator cache (see WmAgentIndicatorRefresh).
static uint8_t g_agentIndicatorState = 0; // BridgePresenceState
static uint32_t g_agentIndicatorPeer = 0;
static uint64_t g_agentIndicatorSilentMs = 0;

static TaskbarLayout GetTaskbarLayout(uint32_t screenW, uint32_t screenH)
{
    char clockText[32];
    RtcFormatTaskbar(clockText, RtcNow(), false);
    char buildText[48]{};
    if (g_buildChip) {
        WmStrCopy(buildText, "Brook ", sizeof(buildText));
        WmStrCopy(buildText + 6, BuildGitHash(), sizeof(buildText) - 6);
    }
    int ids[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity], total = 0, focused = 0;
    const char* labels[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity];
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        const Window& w = g_windows[i];
        if (!w.proc || !w.visible || !w.focusable) continue;
        if (w.focused) focused = total;
        labels[total] = w.title;
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
    return ChromeTaskbarLayoutFor(WmChromeStyle(), screenW, screenH, clockText,
                                  __atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE) != 0,
                                  WmGetTaskbarMetrics(), ids, total, focused, buildText, g_hudMode == WmHudMode::Compact,
                                  labels);
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
    if (!g_volumePopupOpen) {
        if (!WmVolumeIconContains(mx, my, screenW, screenH)) return false;
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
    case TaskbarVolumeHit::Panel: break;
    case TaskbarVolumeHit::Outside: WmVolumePopupClose(); break; // includes the icon itself
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
    if (!delta) return false;
    const bool overPopup = g_volumePopupOpen && WmVolumePopupLayout(screenW, screenH).panel.contains(mx, my);
    if (!overPopup && !WmVolumeIconContains(mx, my, screenW, screenH)) return false;
    WmVolumeApply(TaskbarVolumeWheel(g_volumeControl, delta));
    return true;
}

bool WmVolumeToggleMuteAt(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    const bool overPopup = g_volumePopupOpen && WmVolumePopupLayout(screenW, screenH).panel.contains(mx, my);
    if (!overPopup && !WmVolumeIconContains(mx, my, screenW, screenH)) return false;
    WmVolumeApply(TaskbarVolumeToggleMute(g_volumeControl));
    return true;
}

// Shell painters build their list, then either execute it (key == nullptr)
// or return its content key seeded with keySeed (WmShellSurfaceKey).
static void WmVolumePopupSurface(uint32_t* pixels, uint32_t stride, uint32_t screenW, uint32_t screenH,
                                 int32_t mouseX, int32_t mouseY, uint64_t keySeed, uint64_t* key)
{
    if (!g_volumePopupOpen || (!pixels && !key)) return;
    const ChromeVolumePopupModel model{WmVolumePopupLayout(screenW, screenH), g_volumeControl, g_volumeAvailable,
                                       g_volumeDragging, mouseX, mouseY};
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildVolumePopup(model, WmChromeStyle(), list);
    if (key) *key = ChromeListKey(list, keySeed);
    else ChromeExecute(list, {pixels, stride, screenW, screenH}, WmChromeFonts());
}
void WmVolumePopupRender(uint32_t* pixels, uint32_t stride, uint32_t screenW, uint32_t screenH,
                         int32_t mouseX, int32_t mouseY)
{
    WmVolumePopupSurface(pixels, stride, screenW, screenH, mouseX, mouseY, 0, nullptr);
}

enum class WmTrayFlyout : uint8_t { None, Network, Temperature };
static WmTrayFlyout g_trayFlyout;

bool WmTrayFlyoutVisible() { return g_trayFlyout != WmTrayFlyout::None; }
void WmTrayFlyoutClose() { g_trayFlyout = WmTrayFlyout::None; }
void WmCyclePerformanceHud()
{
    g_hudMode = g_hudMode == WmHudMode::Off ? WmHudMode::Compact :
                g_hudMode == WmHudMode::Compact ? WmHudMode::Panel : WmHudMode::Off;
}

static DesktopRect WmTrayFlyoutLayout(uint32_t sw, uint32_t sh);

bool WmTrayFlyoutPress(int32_t mx, int32_t my, uint32_t sw, uint32_t sh)
{
    if (WmTrayFlyoutLayout(sw, sh).contains(mx, my)) return true;
    const auto l = GetTaskbarLayout(sw, sh);
    if (l.hud.contains(mx, my)) {
        g_hudMode = WmHudMode::Panel;
        WmTrayFlyoutClose();
        return true;
    }
    const auto next = WmTaskbarHidden() ? WmTrayFlyout::None :
                      l.network.contains(mx, my) ? WmTrayFlyout::Network :
                      l.metrics[6].contains(mx, my) ? WmTrayFlyout::Temperature : WmTrayFlyout::None;
    if (!WmTrayFlyoutVisible() && next == WmTrayFlyout::None) return false;
    g_trayFlyout = next == g_trayFlyout ? WmTrayFlyout::None : next;
    if (WmTrayFlyoutVisible()) {
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
    return {int(sw) - w - margin, deskH - h - margin, w, h};
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

bool WmPerformanceHudRect(uint32_t sw, uint32_t sh, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h)
{
    return WmPanelRect(WmPerformanceHudLayout(sw, sh), x, y, w, h);
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

static void WmTrayFlyoutSurface(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh,
                               uint64_t seed, uint64_t* key)
{
    const auto rect = WmTrayFlyoutLayout(sw, sh);
    if (rect.w <= 0) return;
    const bool network = g_trayFlyout == WmTrayFlyout::Network;
    const uint32_t address = __atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE);
    char text[48]{};
    if (network && address) {
        BootText label(text, sizeof(text));
        const auto* bytes = reinterpret_cast<const uint8_t*>(&address);
        for (unsigned i = 0; i < 4; ++i) {
            if (i) label.Add(".");
            label.Number(bytes[i]);
        }
    }
    const ChromeTrayFlyoutModel model{rect, network, address != 0, text, WmTelemetryModel(false, !network)};
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildTrayFlyout(model, WmChromeStyle(), list);
    if (key) *key = ChromeListKey(list, seed);
    else ChromeExecute(list, {pixels, stride, sw, sh}, WmChromeFonts());
}

static void WmPerformanceHudSurface(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh,
                                    uint64_t seed, uint64_t* key)
{
    const auto rect = WmPerformanceHudLayout(sw, sh);
    if (rect.w <= 0) return;
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildPerformanceHud(WmHudModel(rect), WmChromeStyle(), list);
    if (key) *key = ChromeListKey(list, seed);
    else ChromeExecute(list, {pixels, stride, sw, sh}, WmChromeFonts());
}

void WmRenderTrayFlyout(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh)
{
    WmTrayFlyoutSurface(pixels, stride, sw, sh, 0, nullptr);
}

void WmRenderPerformanceHud(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh)
{
    WmPerformanceHudSurface(pixels, stride, sw, sh, 0, nullptr);
}

// Taskbar model: per-button icons and hover labels, tray state and the
// telemetry snapshot. Launch entries copy their icon (owned by the launch
// table under g_launchLock) into launchIcons.
static uint32_t g_taskbarLaunchIcons[LaunchFeedbackTable::Capacity][LAUNCHER_ICON_PX * LAUNCHER_ICON_PX];
static ChromeTaskbarButton g_taskbarButtons[ChromeTaskbarButtonLimit];
static TaskbarLayout g_taskbarLayout;

static void WmTaskbarSurface(uint32_t* backBuffer, uint32_t stride,
                             uint32_t screenW, uint32_t screenH,
                             uint64_t uptimeMs, int32_t mouseX, int32_t mouseY, uint64_t keySeed, uint64_t* key)
{
    if (!g_wmActive || (!backBuffer && !key)) return;
    LauncherReadGuard icons;
    TaskbarLayout& l = g_taskbarLayout;
    l = GetTaskbarLayout(screenW, screenH);
    for (int i = 0; i < l.count; ++i)
    {
        ChromeTaskbarButton& b = g_taskbarButtons[i];
        b = {};
        if (l.indices[i] <= -10) {
            const unsigned slot = static_cast<unsigned>(-10 - l.indices[i]);
            LaunchFeedback e;
            bool hasIcon;
            SpinLockAcquire(&g_launchLock);
            e = g_launches.entries[slot];
            hasIcon = g_launchRequests[slot].hasIcon;
            if (hasIcon) memcpy(g_taskbarLaunchIcons[slot], g_launchRequests[slot].icon, sizeof(g_taskbarLaunchIcons[slot]));
            SpinLockRelease(&g_launchLock);
            b.launch = true;
            b.busy = e.busy();
            b.icon = hasIcon ? g_taskbarLaunchIcons[slot] : icons.get().applicationIcon;
            continue;
        }
        const Window& w = g_windows[l.indices[i]];
        b.active = w.focused && !w.minimized;
        b.minimized = w.minimized;
        b.icon = WindowIcon(w, icons.get());
        b.hoverTitle = w.title;
    }
    ChromeTaskbarModel model{};
    model.layout = &l;
    model.buttons = g_taskbarButtons;
    model.launcherOpen = g_launcherOpen;
    model.appsIcon = icons.get().appsIcon;
    model.terminalIcon = icons.get().terminalIcon;
    model.networkConnected = __atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE) != 0;
    model.volumeControl = g_volumeControl;
    model.volumeAvailable = g_volumeAvailable;
    const uint8_t agent = __atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE);
    model.agentShown = agent != 0;
    model.agentStale = agent == static_cast<uint8_t>(BridgePresenceState::Stale);
    model.telemetry = WmTelemetryModel(l.metrics[5].w != 0, l.metrics[6].w != 0);
    model.uptimeMs = uptimeMs;
    model.mouseX = mouseX; model.mouseY = mouseY;
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    const auto style = WmChromeStyle();
    ChromeBuildTaskbar(model, style, list);
    if (l.hud.w > 0) ChromeBuildCompactHud(WmHudModel(l.hud), style, list);
    if (key) *key = ChromeListKey(list, keySeed);
    else ChromeExecute(list, {backBuffer, stride, screenW, screenH}, WmChromeFonts());
}
void WmRenderTaskbar(uint32_t* backBuffer, uint32_t stride,
                     uint32_t screenW, uint32_t screenH,
                     uint64_t uptimeMs, int32_t mouseX, int32_t mouseY)
{
    WmTaskbarSurface(backBuffer, stride, screenW, screenH, uptimeMs, mouseX, mouseY, 0, nullptr);
}

// Tooltips float above the taskbar, centred on the hovered control. The
// launcher, volume popup and tray flyout take precedence.
static bool WmTrayTooltip(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                          char (&text)[48], const char*& detail, DesktopRect& box)
{
    if (!g_wmActive || g_launcherOpen || g_volumePopupOpen || WmTrayFlyoutVisible() || WmTaskbarHidden()) return false;
    const TaskbarLayout l = GetTaskbarLayout(screenW, screenH);
    const WmRect* anchor = nullptr;
    if (l.volume.contains(mx, my)) {
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
            } else WmStrCopy(text, g_windows[index].title, sizeof(text));
            if (!text[0]) break;
            anchor = &l.buttons[i];
            break;
        }
    }
    if (!anchor && l.build.contains(mx, my)) {
        anchor = &l.build;
        WmStrCopy(text, "Brook ", sizeof(text));
        WmStrCopy(text + 6, BuildGitHash(), sizeof(text) - 6);
        if (g_buildPayloadId[0]) detail = g_buildPayloadId;
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

static void WmTrayTooltipSurface(uint32_t* pixels, uint32_t stride, uint32_t screenW, uint32_t screenH,
                                 int32_t mx, int32_t my, uint64_t keySeed, uint64_t* key)
{
    char text[48]{};
    const char* detail = nullptr;
    DesktopRect p{};
    if ((!pixels && !key) || !WmTrayTooltip(screenW, screenH, mx, my, text, detail, p)) return;
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildTooltip({p, text, detail}, WmChromeStyle(), list);
    if (key) *key = ChromeListKey(list, keySeed);
    else ChromeExecute(list, {pixels, stride, screenW, screenH}, WmChromeFonts());
}
void WmTrayTooltipRender(uint32_t* pixels, uint32_t stride, uint32_t screenW, uint32_t screenH,
                         int32_t mx, int32_t my)
{
    WmTrayTooltipSurface(pixels, stride, screenW, screenH, mx, my, 0, nullptr);
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

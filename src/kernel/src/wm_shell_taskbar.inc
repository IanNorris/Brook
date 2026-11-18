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

// Debug-bridge agent indicator cache (see WmAgentIndicatorRefresh).
static uint8_t g_agentIndicatorState = 0; // BridgePresenceState
static uint32_t g_agentIndicatorPeer = 0;
static uint64_t g_agentIndicatorSilentMs = 0;

static TaskbarLayout GetTaskbarLayout(uint32_t screenW, uint32_t screenH)
{
    char clockText[32];
    RtcFormatTaskbar(clockText, RtcNow(), false);
    int ids[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity], total = 0, focused = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        const Window& w = g_windows[i];
        if (!w.proc || !w.visible || !w.focusable) continue;
        if (w.focused) focused = total;
        ids[total++] = static_cast<int>(i);
    }
    SpinLockAcquire(&g_launchLock);
    for (unsigned i = 0; i < LaunchFeedbackTable::Capacity; ++i) {
        if (!g_launches.entries[i].present()) continue;
        if (g_taskbarPageTarget == -10 - static_cast<int>(i)) focused = total;
        ids[total++] = -10 - static_cast<int>(i);
    }
    SpinLockRelease(&g_launchLock);
    return ChromeTaskbarLayoutFor(WmChromeStyle(), screenW, screenH, clockText,
                                  __atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE) != 0,
                                  WmGetTaskbarMetrics(), ids, total, focused);
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
    WmBuildIdText label{};
    WmRect badge{};
    const bool hasBadge = WmBuildIdLayout(screenW, screenH, label, badge);
    return TaskbarVolumePopup(WmDesktopMetrics(), t.volume, t.y, static_cast<int>(screenW), WmTextWidth("100%"),
                              hasBadge ? &badge : nullptr);
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

void WmVolumePopupRender(uint32_t* pixels, uint32_t stride, uint32_t screenW, uint32_t screenH,
                         int32_t mouseX, int32_t mouseY)
{
    if (!g_volumePopupOpen || !pixels) return;
    const ChromeVolumePopupModel model{WmVolumePopupLayout(screenW, screenH), g_volumeControl, g_volumeAvailable,
                                       g_volumeDragging, mouseX, mouseY};
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildVolumePopup(model, WmChromeStyle(), list);
    ChromeExecute(list, {pixels, stride, screenW, screenH}, WmDesktopFont());
}

// Taskbar model: per-button icons and hover labels, tray state and the
// telemetry snapshot. Launch entries copy their icon (owned by the launch
// table under g_launchLock) into launchIcons.
static uint32_t g_taskbarLaunchIcons[LaunchFeedbackTable::Capacity][LAUNCHER_ICON_PX * LAUNCHER_ICON_PX];
static char g_taskbarLaunchLabels[LaunchFeedbackTable::Capacity][80];
static ChromeTaskbarButton g_taskbarButtons[ChromeTaskbarButtonLimit];
static TaskbarLayout g_taskbarLayout;

void WmRenderTaskbar(uint32_t* backBuffer, uint32_t stride,
                     uint32_t screenW, uint32_t screenH,
                     uint64_t uptimeMs, int32_t mouseX, int32_t mouseY)
{
    if (!g_wmActive || !backBuffer) return;
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
            char* label = g_taskbarLaunchLabels[slot];
            WmStrCopy(label, e.busy() ? "Starting: " :
                e.phase == LaunchPhase::TimedOut ? "Still waiting: " :
                e.phase == LaunchPhase::Exited ? "Exited without a window: " : "Launch failed: ", sizeof(g_taskbarLaunchLabels[slot]));
            const size_t n = strlen(label);
            WmStrCopy(label + n, e.title, sizeof(g_taskbarLaunchLabels[slot]) - n);
            b.launch = true;
            b.busy = e.busy();
            b.icon = hasIcon ? g_taskbarLaunchIcons[slot] : icons.get().applicationIcon;
            b.hoverTitle = label;
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
    model.telemetry = WmTelemetryModel(l);
    model.uptimeMs = uptimeMs;
    model.mouseX = mouseX; model.mouseY = mouseY;
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildTaskbar(model, WmChromeStyle(), list);
    ChromeExecute(list, {backBuffer, stride, screenW, screenH}, WmDesktopFont());
}

// Volume and network tooltips float in a small panel just above the
// taskbar, centred on the hovered icon. They share the desktop overlay slot
// with the launcher and the volume popup, so none shows while either is open.
static bool WmTrayTooltip(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                          char (&text)[48], DesktopRect& box)
{
    if (!g_wmActive || g_launcherOpen || g_volumePopupOpen || WmTaskbarHidden()) return false;
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
    if (!anchor) return false;
    const int pad = ChromeTooltipPadding(WmChromeStyle());
    box = TaskbarTooltipBox(*anchor, WmTextWidth(text) + 2 * pad,
                            WmDesktopFont().lineHeight + static_cast<int>(WmMetric(10)), l.y,
                            static_cast<int>(WmMetric(4)), static_cast<int>(screenW), static_cast<int>(WmMetric(4)));
    return box.w > 0 && box.h > 0;
}

bool WmTrayTooltipGetRect(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                          int32_t* outX, int32_t* outY, uint32_t* outW, uint32_t* outH)
{
    char text[48]{};
    DesktopRect box{};
    if (!WmTrayTooltip(screenW, screenH, mx, my, text, box)) return false;
    *outX = box.x; *outY = box.y; *outW = static_cast<uint32_t>(box.w); *outH = static_cast<uint32_t>(box.h);
    return true;
}

void WmTrayTooltipRender(uint32_t* pixels, uint32_t stride, uint32_t screenW, uint32_t screenH,
                         int32_t mx, int32_t my)
{
    char text[48]{};
    DesktopRect p{};
    if (!pixels || !WmTrayTooltip(screenW, screenH, mx, my, text, p)) return;
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildTooltip({p, text}, WmChromeStyle(), list);
    ChromeExecute(list, {pixels, stride, screenW, screenH}, WmDesktopFont());
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

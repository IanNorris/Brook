// Taskbar, tray indicators, volume popup and tray tooltips: shell layout,
// painters and hit tests. Part of window.cpp's translation unit (it shares
// the WM's window list and launch state); split out unchanged by compositor
// refactor stage 0.3. Include only from window.cpp.

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

#include "desktop_telemetry_runtime.inc"

struct TaskbarLayout
{
    int y, textY;
    WmRect apps, terminal, next, clock, network, volume, agent;
    WmRect metrics[7];
    WmRect buttons[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity];
    int indices[WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity];
    int count, nextIndex;
    char clockText[32];
};

// Debug-bridge agent indicator cache (see WmAgentIndicatorRefresh).
static uint8_t g_agentIndicatorState = 0; // BridgePresenceState
static uint32_t g_agentIndicatorPeer = 0;
static uint64_t g_agentIndicatorSilentMs = 0;

static TaskbarLayout GetTaskbarLayout(uint32_t screenW, uint32_t screenH)
{
    TaskbarLayout l = {};
    l.nextIndex = -1;
    const int sw = static_cast<int>(screenW), sh = static_cast<int>(screenH);
    l.y = sh > static_cast<int>(WmMetric(WM_TASKBAR_HEIGHT)) ? sh - WmMetric(WM_TASKBAR_HEIGHT) : 0;
    const int height = sh - l.y;
    const int bh = height < static_cast<int>(WmMetric(WM_TASKBAR_BTN_HEIGHT)) ? height : WmMetric(WM_TASKBAR_BTN_HEIGHT);
    const int by = l.y + (height - bh) / 2;
    l.textY = by + (bh - WmDesktopFont().lineHeight) / 2;
    int x = WmMetric(WM_TASKBAR_PADDING);
    const int appsW = WmMetric(40);
    l.apps = {x, by, appsW, bh}; x += appsW + WmMetric(WM_TASKBAR_PADDING);
    l.terminal = {x, by, static_cast<int>(WmMetric(40)), bh}; x += WmMetric(40) + WmMetric(WM_TASKBAR_PADDING);
    RtcFormatTaskbar(l.clockText, RtcNow(), false);
    int right = sw - WmMetric(8);
    // Reserve measured widgets from right to left. Keep one useful window
    // button on small displays; diagnostics yield before the clock does.
    auto reserve = [&](WmRect& r, int width) {
        if (right - width - static_cast<int>(WmMetric(12)) < x + static_cast<int>(WmMetric(80))) return;
        right -= width;
        r = {right, by, width, bh};
        right -= WmMetric(12);
    };
    reserve(l.clock, WmTextWidth(l.clockText));
    reserve(l.network, WmMetric(16));
    reserve(l.volume, WmMetric(16));
    // Leftmost fixed icon, so its appearance never moves the volume target.
    if (__atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE)) reserve(l.agent, WmMetric(16));
    const int widths[7]={64,64,112,112,240,240,150};
    const uint32_t enabledMetrics=WmGetTaskbarMetrics();
    for(int i=6;i>=0;--i)if(enabledMetrics&(1u<<i)){
        reserve(l.metrics[i],WmMetric(widths[i]));
        if(l.metrics[i].w){
            // Two 19-pixel text lines do not fit the 32-pixel task buttons.
            // Use the full tray height, retaining a scaled top/bottom inset.
            l.metrics[i].y=l.y+WmMetric(2);
            l.metrics[i].h=height-2*WmMetric(2);
        }
    }
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
    int available = right - x;
    if (!total || available <= 0) return l;
    int capacity = (available + WmMetric(WM_TASKBAR_PADDING)) / (WmMetric(WM_TASKBAR_BTN_WIDTH) + WmMetric(WM_TASKBAR_PADDING));
    bool overflow = capacity < total;
    if (overflow)
    {
        // The arrow focuses the first window on the next page. The focused
        // window selects the page, so every hidden/minimized window remains
        // reachable through the existing focus/restore input path.
        int arrowW = available < static_cast<int>(WmMetric(32)) ? available : static_cast<int>(WmMetric(32));
        l.next = {right - arrowW, by, arrowW, bh};
        available -= arrowW + WmMetric(WM_TASKBAR_PADDING);
        capacity = (available + WmMetric(WM_TASKBAR_PADDING)) / (WmMetric(WM_TASKBAR_BTN_WIDTH) + WmMetric(WM_TASKBAR_PADDING));
        if (capacity < 1 && available > 0) capacity = 1;
        if (capacity < 1) { l.nextIndex = ids[(focused + 1) % total]; return l; }
    }
    if (capacity > total) capacity = total;
    const int first = (focused / capacity) * capacity;
    l.count = total - first < capacity ? total - first : capacity;
    int width = (available - (capacity - 1) * WmMetric(WM_TASKBAR_PADDING)) / capacity;
    if (width > static_cast<int>(WmMetric(WM_TASKBAR_BTN_WIDTH))) width = WmMetric(WM_TASKBAR_BTN_WIDTH);
    for (int i = 0; i < l.count; ++i)
    {
        l.indices[i] = ids[first + i];
        l.buttons[i] = {x, by, width, bh};
        x += width + WmMetric(WM_TASKBAR_PADDING);
    }
    if (overflow) l.nextIndex = ids[(first + capacity) % total];
    return l;
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

// Four ascending bars in the clock colour when connected; dimmed bars with a
// cross over the short (left) end when disconnected.
static void DrawNetworkIndicator(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh,
                                 const WmRect& r, bool connected)
{
    if (r.w <= 0) return;
    const uint32_t fg = WmCurrentPalette().secondaryText;
    const uint32_t bar = connected ? fg : WmBlendColor(fg, WmCurrentPalette().taskbar, 90);
    const int size = static_cast<int>(WmMetric(16));
    const int barW = static_cast<int>(WmMetric(3)), gap = static_cast<int>(WmMetric(1));
    const int x0 = r.x, top = r.y + (r.h - size) / 2, bottom = top + size;
    for (int i = 0; i < 4; ++i) {
        const int h = size * (i + 1) / 4;
        WmFillRect(pixels, stride, sw, sh, x0 + i * (barW + gap), bottom - h, barW, h, bar);
    }
    if (connected) return;
    const int span = size * 5 / 8, t = static_cast<int>(WmMetric(2));
    for (int i = 0; i + t <= span; ++i) {
        WmFillRect(pixels, stride, sw, sh, x0 + i, top + i, t, t, fg);
        WmFillRect(pixels, stride, sw, sh, x0 + span - t - i, top + i, t, t, fg);
    }
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

// A small robot head: antenna, outlined face, two eyes. Connected draws it in
// the clock colour with green eyes; stale dims it, closes the eyes and adds
// an amber "!" badge at the lower right.
static void DrawAgentIndicator(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh, const WmRect& r)
{
    const uint8_t state = __atomic_load_n(&g_agentIndicatorState, __ATOMIC_ACQUIRE);
    if (r.w <= 0 || !state) return;
    const bool stale = state == static_cast<uint8_t>(BridgePresenceState::Stale);
    const uint32_t fg = WmCurrentPalette().secondaryText;
    const uint32_t ink = stale ? WmBlendColor(fg, WmCurrentPalette().taskbar, 120) : fg;
    // Drawn on a 16-unit grid scaled to the 16-pixel metric (any UI scale).
    const int size = static_cast<int>(WmMetric(16));
    const int x0 = r.x + (r.w - size) / 2, y0 = r.y + (r.h - size) / 2;
    auto px = [&](int v) { return v * size / 16; };
    auto box = [&](int x, int y, int w, int h, uint32_t c) {
        WmFillRect(pixels, stride, sw, sh, x0 + px(x), y0 + px(y), px(x + w) - px(x), px(y + h) - px(y), c);
    };
    box(6, 0, 4, 2, ink);           // antenna knob
    box(7, 2, 2, 2, ink);           // antenna stem
    box(1, 4, 14, 2, ink);          // face outline
    box(1, 14, 14, 2, ink);
    box(1, 6, 2, 8, ink);
    box(13, 6, 2, 8, ink);
    if (!stale) {
        const uint32_t eye = BROOK_AGENT_ICON_EYE;
        box(4, 8, 3, 3, eye);
        box(9, 8, 3, 3, eye);
        return;
    }
    box(4, 9, 3, 1, ink);           // closed eyes
    box(9, 9, 3, 1, ink);
    const uint32_t badge = BROOK_AGENT_ICON_BADGE, mark = BROOK_AGENT_ICON_MARK;
    box(10, 9, 6, 7, badge);
    box(12, 10, 2, 3, mark);
    box(12, 14, 2, 1, mark);
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

// Speaker body and cone, then one to three ")" waves for the level, on a
// 16x16 design grid scaled like the network bars. Muted draws a small cross
// where the waves go; no device dims the speaker and crosses it like the
// disconnected network bars.
static void DrawVolumeGlyph(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh,
                            int x0, int y0, int size, uint32_t control, bool available, uint32_t bg)
{
    const uint32_t fg = WmCurrentPalette().secondaryText;
    const uint32_t ink = available ? fg : WmBlendColor(fg, bg, 90);
    auto u = [&](int v) { return v * size / 16; };
    auto cell = [&](int x, int y, int w, int h) {
        WmFillRect(pixels, stride, sw, sh, x0 + u(x), y0 + u(y), u(x + w) - u(x), u(y + h) - u(y), ink);
    };
    cell(0, 5, 3, 6);
    for (int c = 0; c < 3; ++c) cell(3 + c, 4 - c, 1, 8 + 2 * c);
    const TaskbarVolumeGlyph glyph = TaskbarVolumeGlyphFor(control);
    if (glyph == TaskbarVolumeGlyph::Muted) {
        const int span = u(6), t = u(2) > 0 ? u(2) : 1, left = x0 + u(9), top = y0 + u(5);
        for (int i = 0; i + t <= span; ++i) {
            WmFillRect(pixels, stride, sw, sh, left + i, top + i, t, t, ink);
            WmFillRect(pixels, stride, sw, sh, left + span - t - i, top + i, t, t, ink);
        }
    } else {
        const int waves = glyph == TaskbarVolumeGlyph::Low ? 1 : glyph == TaskbarVolumeGlyph::Medium ? 2 : 3;
        for (int i = 0; i < waves; ++i) {
            const int x = 8 + 3 * i, half = 2 + 2 * i, cap = i ? 2 : 1;
            cell(x, 8 - half, 2, 2 * half);
            cell(x - 1, 8 - half - cap, 2, cap);
            cell(x - 1, 8 + half, 2, cap);
        }
    }
    if (available) return;
    const int span = size * 5 / 8, ct = static_cast<int>(WmMetric(2));
    for (int i = 0; i + ct <= span; ++i) {
        WmFillRect(pixels, stride, sw, sh, x0 + i, y0 + i, ct, ct, fg);
        WmFillRect(pixels, stride, sw, sh, x0 + span - ct - i, y0 + i, ct, ct, fg);
    }
}


static void DrawVolumeIndicator(uint32_t* pixels, uint32_t stride, uint32_t sw, uint32_t sh, const WmRect& r)
{
    if (r.w <= 0) return;
    const int size = static_cast<int>(WmMetric(16));
    DrawVolumeGlyph(pixels, stride, sw, sh, r.x, r.y + (r.h - size) / 2, size,
                    g_volumeControl, g_volumeAvailable, WmCurrentPalette().taskbar);
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
    const TaskbarVolumePopupLayout l = WmVolumePopupLayout(screenW, screenH);
    const WmPalette& pal = WmCurrentPalette();
    const DesktopRect& p = l.panel;
    if (p.w <= 2 || p.h <= 2) return;
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y, p.w, p.h, WmMaterialColor(pal.launcher));
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y, p.w, 1, pal.launcherBorder);
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y + p.h - 1, p.w, 1, pal.launcherBorder);
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y, 1, p.h, pal.launcherBorder);
    WmFillRect(pixels, stride, screenW, screenH, p.x + p.w - 1, p.y, 1, p.h, pal.launcherBorder);
    const bool muted = (g_volumeControl & BROOK_AUDIO_MUTED) != 0;
    const uint32_t buttonBg = l.mute.contains(mouseX, mouseY) ? pal.taskHover : pal.taskButton;
    WmMaterialRect(pixels, stride, screenW, screenH, l.mute, buttonBg);
    const int size = static_cast<int>(WmMetric(16));
    DrawVolumeGlyph(pixels, stride, screenW, screenH, l.mute.x + (l.mute.w - size) / 2,
                    l.mute.y + (l.mute.h - size) / 2, size, g_volumeControl, g_volumeAvailable, buttonBg);
    const uint32_t level = brook_audio_volume(g_volumeControl);
    const int barH = static_cast<int>(WmMetric(4)), barY = l.track.y + (l.track.h - barH) / 2;
    const int knobX = TaskbarVolumeSliderX(l.track, level);
    const uint32_t fill = muted || !g_volumeAvailable ? WmBlendColor(pal.secondaryText, pal.launcher, 110) : pal.accent;
    WmFillRect(pixels, stride, screenW, screenH, l.track.x, barY, l.track.w, barH, pal.launcherBorder);
    WmFillRect(pixels, stride, screenW, screenH, l.track.x, barY, knobX - l.track.x + 1, barH, fill);
    const int knob = static_cast<int>(WmMetric(12));
    WmMaterialRect(pixels, stride, screenW, screenH, {knobX - knob / 2, l.track.y + (l.track.h - knob) / 2, knob, knob},
                   g_volumeDragging ? pal.accent : pal.text);
    char value[8]{};
    char* out = value;
    DesktopNumber(out, level);
    while (*out) ++out;
    *out++ = '%'; *out = 0;
    const int textY = l.value.y + (l.value.h - WmDesktopFont().lineHeight) / 2;
    WmRenderMaterialString(pixels, stride, screenW, screenH, l.value.x + l.value.w - WmTextWidth(value), textY,
                           value, muted ? pal.secondaryText : pal.text, pal.launcher, l.value.w);
}

void WmRenderTaskbar(uint32_t* backBuffer, uint32_t stride,
                     uint32_t screenW, uint32_t screenH,
                     uint64_t uptimeMs, int32_t mouseX, int32_t mouseY)
{
    if (!g_wmActive || !backBuffer) return;
    LauncherReadGuard icons;
    const TaskbarLayout l = GetTaskbarLayout(screenW, screenH);
    WmFillRect(backBuffer, stride, screenW, screenH, 0, l.y, screenW,
               screenH - l.y, WmMaterialColor(WmCurrentPalette().taskbar));
    WmFillRect(backBuffer, stride, screenW, screenH, 0, l.y, screenW, 1, WmCurrentPalette().separator);
    auto button = [&](const WmRect& r, const char* title, bool active, bool minimized) {
        uint32_t bg = active ? WmCurrentPalette().taskActive :
            r.contains(mouseX, mouseY) ? WmCurrentPalette().taskHover : WmCurrentPalette().taskButton;
        WmMaterialRect(backBuffer,stride,screenW,screenH,r,bg);
        if (active || minimized)
            WmFillRect(backBuffer, stride, screenW, screenH, r.x + WmMetric(6), r.y + r.h - WmMetric(2),
                       r.w - WmMetric(12), WmMetric(2), active ? WmCurrentPalette().accent : WmCurrentPalette().minimized);
        if (r.w > static_cast<int>(WmMetric(12))) WmRenderString(backBuffer, stride, screenW, screenH,
            r.x + WmMetric(6), l.textY, title, WmCurrentPalette().taskText, bg, r.w - WmMetric(12));
    };
    button(l.apps, "", g_launcherOpen, false);
    button(l.terminal, "", false, false);
    WmDrawIcon(backBuffer, stride, screenW, screenH, l.apps.x + (l.apps.w - static_cast<int>(WmMetric(24))) / 2,
               l.apps.y + (l.apps.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), icons.get().appsIcon);
    WmDrawIcon(backBuffer, stride, screenW, screenH, l.terminal.x + (l.terminal.w - static_cast<int>(WmMetric(24))) / 2,
               l.terminal.y + (l.terminal.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), icons.get().terminalIcon);
    char launchTitle[80]{};
    const uint32_t netAddress = __atomic_load_n(&g_netIndicatorAddress, __ATOMIC_ACQUIRE);
    const char* hoverTitle = l.apps.contains(mouseX, mouseY) ? "Applications" :
        l.terminal.contains(mouseX, mouseY) ? "New terminal" : nullptr;
    for (int i = 0; i < l.count; ++i)
    {
        const WmRect& r = l.buttons[i];
        if (l.indices[i] <= -10) {
            const unsigned slot = static_cast<unsigned>(-10 - l.indices[i]);
            LaunchFeedback e;
            uint32_t icon[LAUNCHER_ICON_PX * LAUNCHER_ICON_PX];
            bool hasIcon;
            SpinLockAcquire(&g_launchLock);
            e = g_launches.entries[slot];
            hasIcon = g_launchRequests[slot].hasIcon;
            if (hasIcon) memcpy(icon, g_launchRequests[slot].icon, sizeof(icon));
            SpinLockRelease(&g_launchLock);
            button(r, "", false, false);
            if (r.w >= static_cast<int>(WmMetric(24)) && r.h >= static_cast<int>(WmMetric(24)))
                WmDrawIcon(backBuffer, stride, screenW, screenH, r.x + (r.w - static_cast<int>(WmMetric(24))) / 2,
                           r.y + (r.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), hasIcon ? icon : icons.get().applicationIcon);
            if (e.busy()) {
                // Eight positions form a bounded animated progress ring.
                static const int dx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
                static const int dy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
                unsigned step = (uptimeMs / 100) % 8;
                for (unsigned dot = 0; dot < 8; ++dot)
                    WmFillRect(backBuffer, stride, screenW, screenH, r.x + r.w - 8 + dx[dot],
                               r.y + r.h - 8 + dy[dot], 2, 2,
                               dot == step ? WmCurrentPalette().taskText : WmCurrentPalette().minimized);
            } else {
                WmRenderString(backBuffer, stride, screenW, screenH, r.x + r.w - WmMetric(10),
                               l.textY, "!", WmCurrentPalette().taskText, WmCurrentPalette().taskButton, WmMetric(10));
            }
            if (r.contains(mouseX, mouseY)) {
                WmStrCopy(launchTitle, e.busy() ? "Starting: " :
                    e.phase == LaunchPhase::TimedOut ? "Still waiting: " :
                    e.phase == LaunchPhase::Exited ? "Exited without a window: " : "Launch failed: ", sizeof(launchTitle));
                size_t n = strlen(launchTitle);
                WmStrCopy(launchTitle + n, e.title, sizeof(launchTitle) - n);
                hoverTitle = launchTitle;
            }
            continue;
        }
        const Window& w = g_windows[l.indices[i]];
        button(r, "", w.focused && !w.minimized, w.minimized);
        if (r.w >= static_cast<int>(WmMetric(24)) && r.h >= static_cast<int>(WmMetric(24)))
            WmDrawIcon(backBuffer, stride, screenW, screenH, r.x + (r.w - static_cast<int>(WmMetric(24))) / 2,
                       r.y + (r.h - static_cast<int>(WmMetric(24))) / 2, WmMetric(24), WindowIcon(w, icons.get()));
        if (r.contains(mouseX, mouseY)) hoverTitle = w.title;
    }
    if (l.next.w > 0) {
        button(l.next, ">", false, false);
        if (l.next.contains(mouseX, mouseY)) hoverTitle = "More windows";
    }
    // Use the already-reserved tray for the hover label. Geometry stays fixed
    // and no additional empty strip is reserved when nothing is hovered.
    int labelX = static_cast<int>(screenW);
    const WmRect trayRects[] = {l.metrics[0],l.metrics[1],l.metrics[2],l.metrics[3],l.metrics[4],l.metrics[5],l.metrics[6],l.agent,l.volume,l.network,l.clock};
    for (const auto& r : trayRects) if (r.w && r.x < labelX) labelX = r.x;
    if (hoverTitle && labelX + static_cast<int>(WmMetric(12)) < static_cast<int>(screenW))
    {
        WmRenderString(backBuffer, stride, screenW, screenH, labelX, l.textY,
                       hoverTitle, WmCurrentPalette().text, WmCurrentPalette().taskbar, static_cast<int>(screenW) - labelX - WmMetric(8));
        return;
    }
    auto trayText = [&](const WmRect& r, const char* text, uint32_t fg) {
        if (r.w > 0) WmRenderString(backBuffer, stride, screenW, screenH,
            r.x, l.textY, text, fg, WmCurrentPalette().taskbar, r.w);
    };
    trayText(l.clock, l.clockText, WmCurrentPalette().secondaryText);
    DrawNetworkIndicator(backBuffer, stride, screenW, screenH, l.network, netAddress != 0);
    DrawVolumeIndicator(backBuffer, stride, screenW, screenH, l.volume);
    DrawAgentIndicator(backBuffer, stride, screenW, screenH, l.agent);
    for(unsigned i=0;i<7;++i)DrawDesktopMetric(backBuffer,stride,screenW,screenH,l.metrics[i],i,uptimeMs);
}

// Volume and network tooltips float in a small panel just above the
// taskbar, centred on the hovered icon. They share the desktop overlay slot
// with the launcher and the volume popup, so none shows while either is open.
static bool WmTrayTooltip(uint32_t screenW, uint32_t screenH, int32_t mx, int32_t my,
                          char (&text)[48], DesktopRect& box)
{
    if (!g_wmActive || g_launcherOpen || g_volumePopupOpen) return false;
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
    const int pad = static_cast<int>(WmMetric(8));
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
    const WmPalette& pal = WmCurrentPalette();
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y, p.w, p.h, WmMaterialColor(pal.launcher));
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y, p.w, 1, pal.launcherBorder);
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y + p.h - 1, p.w, 1, pal.launcherBorder);
    WmFillRect(pixels, stride, screenW, screenH, p.x, p.y, 1, p.h, pal.launcherBorder);
    WmFillRect(pixels, stride, screenW, screenH, p.x + p.w - 1, p.y, 1, p.h, pal.launcherBorder);
    const int pad = static_cast<int>(WmMetric(8));
    WmRenderMaterialString(pixels, stride, screenW, screenH, p.x + pad,
                           p.y + (p.h - WmDesktopFont().lineHeight) / 2, text, pal.text, pal.launcher, p.w - pad);
}

int WmTaskbarHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (mx < 0 || my < 0 || mx >= static_cast<int32_t>(screenW) || my >= static_cast<int32_t>(screenH)) return -1;
    const TaskbarLayout l = GetTaskbarLayout(screenW, screenH);
    if (l.apps.contains(mx, my)) return -3;
    if (l.terminal.contains(mx, my)) return -2;
    if (l.next.contains(mx, my)) return l.nextIndex <= -10 ? l.nextIndex - 90 : l.nextIndex;
    for (int i = 0; i < l.count; ++i)
        if (l.buttons[i].contains(mx, my)) return l.indices[i];
    return -1;
}

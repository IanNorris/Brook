// Launcher popup: state snapshot, layout, scrolling and hit test. The
// layout and display list builder are freestanding (chrome_shell.cpp); this
// part of window.cpp's translation unit (it shares the launcher catalog and
// state) snapshots the model. Include only from
// window.cpp.

using LauncherLayout = ChromeLauncherLayout;

static LauncherLayout GetLauncherLayout(uint32_t screenW, uint32_t screenH,
                                        const LauncherSnapshot& launcher)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < launcher.count; ++i)
        if (launcher.items[i].valid) ++total;
    const LauncherLayout l = ChromeLauncherLayoutFor(WmChromeStyle(), screenW, screenH, total);
    uint32_t maxScroll = total > l.capacity ? total - l.capacity : 0;
    if (g_launcherScroll > maxScroll) g_launcherScroll = maxScroll;
    return l;
}

static LauncherPowerLayout LauncherPowerLayoutFor(const LauncherLayout& l)
{
    return ChromeLauncherPowerLayout(WmChromeStyle(), l);
}

static bool LauncherShutdownActive() { return !ShutdownLaunchAllowed(); }

static WmRect LauncherItemRect(const LauncherLayout& l, uint32_t slot)
{
    return ChromeLauncherItemRect(WmChromeStyle(), l, slot);
}

void WmLauncherScroll(int delta, uint32_t screenW, uint32_t screenH)
{
    if (!g_launcherOpen) return;
    LauncherReadGuard guard;
    const auto& launcher = guard.get();
    uint32_t total = 0;
    for (uint32_t i = 0; i < launcher.count; ++i)
        if (launcher.items[i].valid) ++total;
    const LauncherLayout l = GetLauncherLayout(screenW, screenH, launcher);
    if (!l.capacity || total <= l.capacity) return;
    if (delta < 0 && g_launcherScroll < total - l.capacity) ++g_launcherScroll;
    else if (delta > 0 && g_launcherScroll) --g_launcherScroll;
}

static void LauncherGetRect(uint32_t screenW, uint32_t screenH,
                            int32_t* outX, int32_t* outY,
                            uint32_t* outW, uint32_t* outH, const LauncherSnapshot& launcher)
{
    const LauncherLayout l = GetLauncherLayout(screenW, screenH, launcher);
    *outX = l.x; *outY = l.y; *outW = l.w; *outH = l.h;
}

void WmLauncherGetRect(uint32_t screenW, uint32_t screenH,
                       int32_t* outX, int32_t* outY,
                       uint32_t* outW, uint32_t* outH)
{
    LauncherReadGuard guard;
    LauncherGetRect(screenW, screenH, outX, outY, outW, outH, guard.get());
}

static void WmCaptureLauncher(uint32_t screenW, uint32_t screenH, int32_t mouseX, int32_t mouseY,
                              ChromeShellModel& snapshot)
{
    if (!g_launcherLoaded) return;
    // A closed launcher can prepare exactly the default page it will show
    // on open. Hover, scroll, errors and confirmations only belong to the
    // open session.
    const int error = g_launcherOpen ? g_launcherError : 0;
    LauncherReadGuard guard;
    const auto& launcher = guard.get();

    snapshot.kind = ChromeShellKind::Launcher;
    snapshot.launcher = GetLauncherLayout(screenW, screenH, launcher);
    const uint32_t scroll = g_launcherOpen ? g_launcherScroll : 0;
    snapshot.shutdownActive = LauncherShutdownActive();
    snapshot.prompt = g_launcherOpen && g_launcherPower.prompt();
    snapshot.promptRestart = g_launcherOpen && g_launcherPower.pending() == LauncherPowerAction::Restart;
    snapshot.confirmFocused = g_launcherOpen && g_launcherPower.confirmFocused();
    const char* header = error == -2 ? "App launch already pending" : error ? "Launch queue full" :
        snapshot.prompt && !snapshot.shutdownActive ?
            (snapshot.promptRestart ? "Restart? Open apps will be asked to close" :
                                      "Shut down? Open apps will be asked to close") :
        ShutdownDesktopStatus();
    WmStrCopy(snapshot.text, header, sizeof(snapshot.text));

    uint32_t totalValid = 0;
    for (uint32_t i = 0; i < launcher.count; ++i) {
        const auto& item = launcher.items[i];
        if (!item.valid) continue;
        if (totalValid >= scroll && snapshot.launcherCount < snapshot.launcher.capacity &&
            snapshot.launcherCount < ChromeShellModel::LauncherLimit) {
            const uint32_t slot = snapshot.launcherCount++;
            WmStrCopy(snapshot.entries[slot].label, item.title, sizeof(snapshot.entries[slot].label));
            WmCaptureShellIcon(snapshot.icons[slot], item.iconPixels ? item.iconPixels : launcher.applicationIcon);
        }
        ++totalValid;
    }
    snapshot.scrollUp = scroll > 0;
    snapshot.scrollDown = scroll + snapshot.launcher.capacity < totalValid;
    const auto& layout = snapshot.launcher;
    const DesktopRect bounds{layout.x, layout.y, int(layout.w), int(layout.h)};
    const bool hovered = g_launcherOpen && bounds.contains(mouseX, mouseY);
    snapshot.mouseX = hovered ? mouseX : -1;
    snapshot.mouseY = hovered ? mouseY : -1;
}

int WmLauncherHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    LauncherReadGuard guard;
    const auto& launcher = guard.get();
    if (!g_launcherOpen) return -1;

    int32_t px, py;
    uint32_t pw, ph;
    LauncherGetRect(screenW, screenH, &px, &py, &pw, &ph, launcher);

    // Outside panel?
    if (mx < px || mx >= px + static_cast<int32_t>(pw) ||
        my < py || my >= py + static_cast<int32_t>(ph))
        return -1;

    const LauncherLayout layout = GetLauncherLayout(screenW, screenH, launcher);
    const LauncherPowerTarget power = LauncherPowerHit(LauncherPowerLayoutFor(layout), g_launcherPower.prompt(),
                                                       LauncherShutdownActive(), mx, my);
    if (power != LauncherPowerTarget::None)
        return WM_LAUNCHER_MAX_ITEMS + static_cast<int>(power); // inert Footer is consumed, not a dismissal
    uint32_t maxVisible = layout.capacity;

    // Map visual slot to actual launcher item, accounting for scroll
    uint32_t validIdx = 0;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < launcher.count && slot < maxVisible; i++)
    {
        if (!launcher.items[i].valid) continue;
        if (validIdx < g_launcherScroll) { validIdx++; continue; }
        validIdx++;

        if (LauncherItemRect(layout, slot).contains(mx, my))
        {
            return static_cast<int>(i);
        }
        slot++;
    }

    return -1; // clicked in panel but not on an item (header or padding)
}

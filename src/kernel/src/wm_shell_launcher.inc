// Launcher popup: state snapshot, layout, scrolling and hit test. The
// layout and display list builder are freestanding (chrome_shell.cpp); this
// part of window.cpp's translation unit (it shares the launcher catalog and
// state) snapshots the model and executes the list. Include only from
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

static void WmLauncherSurface(uint32_t* backBuffer, uint32_t stride,
                              uint32_t screenW, uint32_t screenH,
                              int32_t mouseX, int32_t mouseY, uint64_t keySeed, uint64_t* key)
{
    LauncherReadGuard guard;
    const auto& launcher = guard.get();
    if (!g_launcherOpen || (!backBuffer && !key)) return;

    // Compositor thread only: the shell surfaces share one list storage.
    static ChromeLauncherItem items[ChromeShellMetrics::LauncherMaxRows * ChromeShellMetrics::LauncherMaxCols];
    const ChromeStyle style = WmChromeStyle();
    ChromeLauncherModel model{};
    model.layout = GetLauncherLayout(screenW, screenH, launcher);
    const bool shutdownActive = LauncherShutdownActive();
    model.header = g_launcherError == -2 ? "App launch already pending" : g_launcherError ? "Launch queue full" :
        g_launcherPower.prompt() && !shutdownActive ?
            (g_launcherPower.pending() == LauncherPowerAction::Restart ?
                 "Restart? Open apps will be asked to close" : "Shut down? Open apps will be asked to close") :
        ShutdownDesktopStatus();
    uint32_t totalValid = 0;
    for (uint32_t i = 0; i < launcher.count; i++)
    {
        if (!launcher.items[i].valid) continue;
        // Items before the scroll offset are skipped; the page holds capacity.
        if (totalValid >= g_launcherScroll && model.count < model.layout.capacity &&
            model.count < sizeof(items) / sizeof(items[0]))
            items[model.count++] = {launcher.items[i].title,
                                    launcher.items[i].iconPixels ? launcher.items[i].iconPixels : launcher.applicationIcon};
        totalValid++;
    }
    model.items = items;
    model.scrollUp = g_launcherScroll > 0;
    model.scrollDown = g_launcherScroll + model.layout.capacity < totalValid;
    model.shutdownActive = shutdownActive;
    model.prompt = g_launcherPower.prompt();
    model.promptRestart = g_launcherPower.pending() == LauncherPowerAction::Restart;
    model.confirmFocused = g_launcherPower.confirmFocused();
    model.mouseX = mouseX; model.mouseY = mouseY;
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildLauncher(model, style, list);
    if (key) *key = ChromeListKey(list, keySeed);
    else ChromeExecute(list, {backBuffer, stride, screenW, screenH}, WmDesktopFont());
}
void WmLauncherRender(uint32_t* backBuffer, uint32_t stride,
                      uint32_t screenW, uint32_t screenH,
                      int32_t mouseX, int32_t mouseY)
{
    WmLauncherSurface(backBuffer, stride, screenW, screenH, mouseX, mouseY, 0, nullptr);
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

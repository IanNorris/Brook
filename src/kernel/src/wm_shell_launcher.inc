// Launcher popup: layout, scrolling, painter (including the power footer and
// its confirmation) and hit test. Part of window.cpp's translation unit (it
// shares the launcher catalog and state); split out unchanged by compositor
// refactor stage 0.3. Include only from window.cpp.

struct LauncherLayout
{
    int x, y;
    uint32_t w, h, cols, rows, itemW, capacity;
};

static constexpr const char* LAUNCHER_POWER_LABELS[] = {"Restart", "Shut down", "Cancel"};
static constexpr const char* LAUNCHER_CONFIRM_LABELS[] = {"Restart now", "Shut down now"};

static LauncherPowerMetrics LauncherPowerMetricsNow()
{
    int label = 0, confirm = 0;
    for (const char* text : LAUNCHER_POWER_LABELS) { const int w = WmTextWidth(text); if (w > label) label = w; }
    for (const char* text : LAUNCHER_CONFIRM_LABELS) { const int w = WmTextWidth(text); if (w > confirm) confirm = w; }
    const int inner = 2 * static_cast<int>(WmMetric(14));
    return {static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT)), static_cast<int>(WmMetric(LAUNCHER_PADDING)),
            static_cast<int>(WmMetric(6)), static_cast<int>(WmMetric(28)), static_cast<int>(WmMetric(12)),
            label + inner, confirm + inner};
}

static LauncherLayout GetLauncherLayout(uint32_t screenW, uint32_t screenH,
                                        const LauncherSnapshot& launcher)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < launcher.count; ++i)
        if (launcher.items[i].valid) ++total;
    const uint32_t desktopH = screenH > WmMetric(WM_TASKBAR_HEIGHT) ? screenH - WmMetric(WM_TASKBAR_HEIGHT) : 0;
    LauncherLayout l = {};
    l.x = screenW > WmMetric(8) ? WmMetric(4) : 0;
    const uint32_t availableW = screenW > WmMetric(8) ? screenW - WmMetric(8) : screenW;
    const uint32_t availableH = desktopH > WmMetric(4) ? desktopH - WmMetric(4) : desktopH;
    const LauncherPowerMetrics power = LauncherPowerMetricsNow();
    const uint32_t fixedH = WmMetric(LAUNCHER_ITEM_HEIGHT) + 2 * WmMetric(LAUNCHER_PADDING) +
                            static_cast<uint32_t>(LauncherPowerFooterHeight(power));
    l.rows = availableH > fixedH ? (availableH - fixedH) / (WmMetric(LAUNCHER_ITEM_HEIGHT) + WmMetric(2)) : 0;
    if (l.rows > LAUNCHER_MAX_ROWS) l.rows = LAUNCHER_MAX_ROWS;
    l.cols = l.rows ? (total + l.rows - 1) / l.rows : 1;
    if (!l.cols) l.cols = 1;
    if (l.cols > LAUNCHER_MAX_COLS) l.cols = LAUNCHER_MAX_COLS;
    uint32_t fitCols = availableW > 2 * WmMetric(LAUNCHER_PADDING) ?
        (availableW - 2 * WmMetric(LAUNCHER_PADDING)) / (WmMetric(LAUNCHER_ITEM_WIDTH) + WmMetric(4)) : 0;
    if (!fitCols) fitCols = 1;
    if (l.cols > fitCols) l.cols = fitCols;
    l.itemW = availableW > 2 * WmMetric(LAUNCHER_PADDING) + WmMetric(4) ?
        (availableW - 2 * WmMetric(LAUNCHER_PADDING)) / l.cols - WmMetric(4) : 0;
    if (l.itemW > WmMetric(LAUNCHER_ITEM_WIDTH)) l.itemW = WmMetric(LAUNCHER_ITEM_WIDTH);
    uint32_t neededRows = (total + l.cols - 1) / l.cols;
    if (l.rows > neededRows) l.rows = neededRows;
    l.capacity = l.rows * l.cols;
    uint32_t maxScroll = total > l.capacity ? total - l.capacity : 0;
    if (g_launcherScroll > maxScroll) g_launcherScroll = maxScroll;
    l.w = l.cols * (l.itemW + WmMetric(4)) + 2 * WmMetric(LAUNCHER_PADDING);
    const uint32_t footerW = static_cast<uint32_t>(LauncherPowerMinimumWidth(power));
    if (l.w < footerW) l.w = footerW; // the power prompt never clips its confirm button
    if (l.w > availableW) l.w = availableW;
    l.h = fixedH + l.rows * (WmMetric(LAUNCHER_ITEM_HEIGHT) + WmMetric(2));
    if (l.h > availableH) l.h = availableH;
    l.y = static_cast<int>(desktopH) - static_cast<int>(l.h) - WmMetric(2);
    if (l.y < 0) l.y = 0;
    return l;
}

static LauncherPowerLayout LauncherPowerLayoutFor(const LauncherLayout& l)
{
    return LauncherPowerFooter({l.x, l.y, static_cast<int>(l.w), static_cast<int>(l.h)}, LauncherPowerMetricsNow());
}

static void LauncherPowerButton(uint32_t* buf, uint32_t stride, uint32_t sw, uint32_t sh, const WmRect& r,
                                const char* label, uint32_t bg, uint32_t fg, bool focused)
{
    if (r.w <= 0 || r.h <= 0) return;
    const int inset = static_cast<int>(WmMetric(4));
    const WmRect face{r.x, r.y + inset, r.w, r.h - 2 * inset};
    if (focused) {
        const int ring = static_cast<int>(WmMetric(2));
        WmMaterialRect(buf, stride, sw, sh, {face.x - ring, face.y - ring, face.w + 2 * ring, face.h + 2 * ring}, WmCurrentPalette().accent);
    }
    WmMaterialRect(buf, stride, sw, sh, face, bg);
    const int textW = WmTextWidth(label);
    const int x = r.x + (textW < r.w ? (r.w - textW) / 2 : 0);
    WmRenderMaterialString(buf, stride, sw, sh, x, r.y + (r.h - WmDesktopFont().lineHeight) / 2, label, fg, bg, r.w);
}

static bool LauncherShutdownActive() { return !ShutdownLaunchAllowed(); }

static void LauncherPowerRender(uint32_t* buf, uint32_t stride, uint32_t sw, uint32_t sh,
                                const LauncherLayout& layout, int32_t mouseX, int32_t mouseY)
{
    const LauncherPowerLayout f = LauncherPowerLayoutFor(layout);
    if (f.footer.w <= 0) return;
    const WmPalette& pal = WmCurrentPalette();
    WmFillRect(buf, stride, sw, sh, f.separator.x, f.separator.y, f.separator.w, f.separator.h, pal.launcherBorder);
    const auto face = [&](const DesktopRect& r) { return r.contains(mouseX, mouseY) ? pal.taskHover : pal.taskButton; };
    if (LauncherShutdownActive()) {
        LauncherPowerButton(buf, stride, sw, sh, f.cluster, "Cancel shutdown", face(f.cluster), pal.text, false);
        return;
    }
    if (g_launcherPower.prompt()) {
        const bool restart = g_launcherPower.pending() == LauncherPowerAction::Restart;
        const bool focusConfirm = g_launcherPower.confirmFocused();
        // Destructive confirm: the close-button red, apart from the cluster.
        const uint32_t confirmBg = f.confirm.contains(mouseX, mouseY) ? WmBlendColor(pal.closeHover, pal.text, 200) : pal.closeHover;
        LauncherPowerButton(buf, stride, sw, sh, f.confirm, LAUNCHER_CONFIRM_LABELS[restart ? 0 : 1], confirmBg, pal.closeText, focusConfirm);
        LauncherPowerButton(buf, stride, sw, sh, f.dismiss, LAUNCHER_POWER_LABELS[2], face(f.dismiss), pal.text, !focusConfirm);
        return;
    }
    LauncherPowerButton(buf, stride, sw, sh, f.restart, LAUNCHER_POWER_LABELS[0], face(f.restart), pal.text, false);
    LauncherPowerButton(buf, stride, sw, sh, f.shutdown, LAUNCHER_POWER_LABELS[1], face(f.shutdown), pal.text, false);
}

static WmRect LauncherItemRect(const LauncherLayout& l, uint32_t slot)
{
    return {l.x + static_cast<int>(WmMetric(LAUNCHER_PADDING) + (slot % l.cols) * (l.itemW + WmMetric(4))),
            l.y + static_cast<int>(WmMetric(LAUNCHER_PADDING) + WmMetric(LAUNCHER_ITEM_HEIGHT) +
                                   (slot / l.cols) * (WmMetric(LAUNCHER_ITEM_HEIGHT) + WmMetric(2))),
            static_cast<int>(l.itemW), static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT))};
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

void WmLauncherRender(uint32_t* backBuffer, uint32_t stride,
                      uint32_t screenW, uint32_t screenH,
                      int32_t mouseX, int32_t mouseY)
{
    LauncherReadGuard guard;
    const auto& launcher = guard.get();
    if (!g_launcherOpen) return;

    int32_t px, py;
    uint32_t pw, ph;
    LauncherGetRect(screenW, screenH, &px, &py, &pw, &ph, launcher);

    // Panel background
    WmFillRect(backBuffer, stride, screenW, screenH,
               px, py, static_cast<int>(pw), static_cast<int>(ph), WmMaterialColor(WmCurrentPalette().launcher));

    // Border
    WmFillRect(backBuffer, stride, screenW, screenH, px, py, static_cast<int>(pw), 1, WmCurrentPalette().launcherBorder);
    WmFillRect(backBuffer, stride, screenW, screenH, px, py + static_cast<int32_t>(ph) - 1, static_cast<int>(pw), 1, WmCurrentPalette().launcherBorder);
    WmFillRect(backBuffer, stride, screenW, screenH, px, py, 1, static_cast<int>(ph), WmCurrentPalette().launcherBorder);
    WmFillRect(backBuffer, stride, screenW, screenH, px + static_cast<int32_t>(pw) - 1, py, 1, static_cast<int>(ph), WmCurrentPalette().launcherBorder);

    int textYOff = (static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT)) - WmDesktopFont().lineHeight) / 2;

    // Extremely small surfaces cannot fit even the header.
    if (ph < WmMetric(LAUNCHER_ITEM_HEIGHT) + 2 * WmMetric(LAUNCHER_PADDING) || pw < 2 * WmMetric(LAUNCHER_PADDING) + 1) return;

    // Header: "Applications"
    int32_t iy = py + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING));
    if (pw > WmMetric(56)) WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                   px + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)) + static_cast<int>(WmMetric(4)),
                   iy + static_cast<int32_t>(textYOff),
                   g_launcherError == -2 ? "App launch already pending" : g_launcherError ? "Launch queue full" :
                   g_launcherPower.prompt() && !LauncherShutdownActive() ?
                       (g_launcherPower.pending() == LauncherPowerAction::Restart ?
                            "Restart? Open apps will be asked to close" : "Shut down? Open apps will be asked to close") :
                   ShutdownDesktopStatus(), WmCurrentPalette().secondaryText, WmCurrentPalette().launcher, static_cast<int>(pw) - WmMetric(56));
    iy += WmMetric(LAUNCHER_ITEM_HEIGHT);

    // Separator line below header
    WmFillRect(backBuffer, stride, screenW, screenH,
               px + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)),
               iy - 1, static_cast<int>(pw) - 2 * WmMetric(LAUNCHER_PADDING), 1, WmCurrentPalette().launcherBorder);

    // Items — render in multi-column layout based on scroll offset
    uint32_t totalValid = 0;
    for (uint32_t i = 0; i < launcher.count; i++)
        if (launcher.items[i].valid) totalValid++;

    const LauncherLayout layout = GetLauncherLayout(screenW, screenH, launcher);
    // Power controls: always available, in their own footer below a separator.
    LauncherPowerRender(backBuffer, stride, screenW, screenH, layout, mouseX, mouseY);
    uint32_t maxVisible = layout.capacity;

    uint32_t validIdx = 0;
    uint32_t rendered = 0;
    for (uint32_t i = 0; i < launcher.count && rendered < maxVisible; i++)
    {
        if (!launcher.items[i].valid) continue;

        // Skip items before scroll offset
        if (validIdx < g_launcherScroll)
        {
            validIdx++;
            continue;
        }
        validIdx++;

        const WmRect item = LauncherItemRect(layout, rendered);
        const int itemX = item.x, itemY = item.y;
        bool hovered = item.contains(mouseX, mouseY);
        uint32_t itemBg = WmMaterialColor(hovered ? WmCurrentPalette().itemHover : WmCurrentPalette().launcher);

        WmFillRect(backBuffer, stride, screenW, screenH,
                   itemX, itemY,
                   static_cast<int>(layout.itemW),
                   static_cast<int>(WmMetric(LAUNCHER_ITEM_HEIGHT)),
                   itemBg);

        // Draw icon
        int32_t iconX = itemX + WmMetric(6);
        int32_t iconY = itemY + static_cast<int32_t>((WmMetric(LAUNCHER_ITEM_HEIGHT) - WmMetric(LAUNCHER_ICON_SIZE)) / 2);
        if (layout.itemW >= WmMetric(LAUNCHER_ICON_SIZE) + WmMetric(12))
            LauncherDrawIcon(backBuffer, stride, screenW, screenH,
                             iconX, iconY, &launcher.items[i], launcher);

        // Title text (offset to right of icon)
        int32_t textX = iconX + static_cast<int32_t>(WmMetric(LAUNCHER_ICON_SIZE)) +
                        static_cast<int32_t>(WmMetric(LAUNCHER_ICON_MARGIN));
        uint32_t textFg = hovered ? WmCurrentPalette().text : WmCurrentPalette().taskText;
        int maxTextW = static_cast<int>(layout.itemW) - static_cast<int>(WmMetric(LAUNCHER_ICON_SIZE) + WmMetric(LAUNCHER_ICON_MARGIN) + WmMetric(12));
        if (maxTextW > 0) WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                       textX, itemY + static_cast<int32_t>(textYOff),
                       launcher.items[i].title,
                       textFg, itemBg, maxTextW);

        rendered++;
    }

    // Scroll indicators (arrows) if content overflows
    if (pw >= WmMetric(48) && g_launcherScroll > 0)
    {
        // Wheel-scroll hints live in the header, clear of application labels.
        WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                       px + static_cast<int32_t>(pw) - static_cast<int>(WmMetric(32)),
                       py + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)) + static_cast<int32_t>(textYOff),
                       "^", WmCurrentPalette().secondaryText, WmCurrentPalette().launcher);
    }
    if (pw >= WmMetric(48) && g_launcherScroll + maxVisible < totalValid)
    {
        // Down arrow beside the up hint.
        WmRenderMaterialString(backBuffer, stride, screenW, screenH,
                       px + static_cast<int32_t>(pw) - static_cast<int>(WmMetric(16)),
                       py + static_cast<int32_t>(WmMetric(LAUNCHER_PADDING)) + textYOff,
                       "v", WmCurrentPalette().secondaryText, WmCurrentPalette().launcher);
    }
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

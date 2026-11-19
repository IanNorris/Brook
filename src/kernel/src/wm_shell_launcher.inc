// Launcher glue: the WM side of the Clearwater launcher. The model
// (launcher_model.h) and the surface builder (chrome_launcher.h) are
// freestanding; this part of window.cpp's translation unit (it shares the
// launcher catalog, the power guard and the launch queue) snapshots the
// catalog into the model, routes input, persists the layout and executes the
// list. Include only from window.cpp. Compositor thread, except where noted.

// Model app -> catalog item in the active snapshot, and its icon (borrowed
// from that snapshot; resynchronised whenever the active snapshot changes).
static uint16_t g_launcherModelItem[LauncherLimits::Apps];
static const uint32_t* g_launcherModelIcon[LauncherLimits::Apps];
static uint32_t g_launcherModelGeneration = 0;
static ChromeLauncherFrame g_launcherFrame;

// Persistence: the compositor serialises a changed layout; the launcher
// worker writes it, at most once a second, as a temporary file and a rename.
static constexpr const char* LauncherLayoutPath = "/data/home/.config/brook/launcher.ini";
static constexpr const char* LauncherLayoutTemp = "/data/home/.config/brook/launcher.ini.tmp";
static constexpr uint32_t LauncherLayoutLimit = 16 * 1024;
static SpinLock g_launcherSaveLock;
static char g_launcherSaveText[LauncherLayoutLimit];
static uint32_t g_launcherSaveBytes = 0;
static bool g_launcherSavePending = false;
static uint64_t g_launcherSaveChanged = 0;
static uint32_t g_launcherSavedEpoch = 0;

// The model's stable key for a catalog item: the app_id, or the shortcut
// file name ("/boot/SHORTCUTS/QUAKE.RC" -> "quake"), or a desktop title.
static void LauncherItemKey(const LauncherItem& item, char (&key)[LauncherLimits::KeyLength])
{
    if (item.appId[0]) { WmStrCopy(key, item.appId, sizeof(key)); return; }
    if (item.isDesktopEntry) { WmStrCopy(key, item.title, sizeof(key)); return; }
    const char* name = item.scriptPath;
    for (const char* p = item.scriptPath; *p; ++p) if (*p == '/') name = p + 1;
    uint32_t n = 0;
    for (; name[n] && name[n] != '.' && n + 1 < sizeof(key); ++n)
        key[n] = name[n] >= 'A' && name[n] <= 'Z' ? char(name[n] + 32) : name[n] == ' ' ? '-' : name[n];
    key[n] = 0;
    if (!key[0]) WmStrCopy(key, item.title, sizeof(key));
}

// Shell context (WmLauncherLoad) or compositor thread (publication, render).
static void LauncherSyncModel(const LauncherSnapshot& catalog)
{
    static LauncherApp apps[LauncherLimits::Apps];
    uint32_t count = 0;
    for (uint32_t i = 0; i < catalog.count && count < LauncherLimits::Apps; ++i) {
        const LauncherItem& item = catalog.items[i];
        if (!item.valid) continue;
        LauncherApp& app = apps[count];
        app = {};
        LauncherItemKey(item, app.key);
        // Two items with one key: the later one falls back to its catalog ID.
        bool duplicate = false;
        for (uint32_t j = 0; j < count && !duplicate; ++j) duplicate = !strcmp(apps[j].key, app.key);
        if (duplicate) WmStrCopy(app.key, item.catalogId, sizeof(app.key));
        WmStrCopy(app.name, item.title, sizeof(app.name));
        WmStrCopy(app.desc, item.description, sizeof(app.desc));
        WmStrCopy(app.hint, item.category, sizeof(app.hint));
        g_launcherModelItem[count] = static_cast<uint16_t>(i);
        g_launcherModelIcon[count] = item.iconPixels ? item.iconPixels : catalog.applicationIcon;
        ++count;
    }
    g_launcherModel.SetCatalog(apps, count);
    g_launcherModelGeneration = catalog.generation;
}

// Shell context, once, before the first catalog: a bad or missing file keeps
// the defaults (never an error).
static void LauncherLoadLayout()
{
    g_launcherModel.Init();
    Vnode* vn = VfsOpen(LauncherLayoutPath, 0);
    if (!vn) return;
    char* text = static_cast<char*>(kmalloc(LauncherLayoutLimit + 1));
    uint32_t done = 0;
    bool ok = text != nullptr;
    if (ok) {
        uint64_t offset = 0;
        while (done < LauncherLayoutLimit) {
            const int rd = VfsRead(vn, text + done, LauncherLayoutLimit - done, &offset);
            if (rd < 0) { ok = false; break; }
            if (!rd) break;
            done += static_cast<uint32_t>(rd);
        }
    }
    VfsClose(vn);
    if (ok && done < LauncherLayoutLimit) {
        text[done] = 0;
        SerialPrintf("WM: launcher layout %s (%u bytes)\n", g_launcherModel.Parse(text) ? "restored" : "invalid; defaults", done);
    }
    kfree(text);
}

// Compositor thread: hand a changed layout to the worker.
static void LauncherNoteLayout()
{
    const uint32_t epoch = g_launcherModel.LayoutEpoch();
    if (epoch == g_launcherSavedEpoch) return;
    static char text[LauncherLayoutLimit];
    const uint32_t bytes = g_launcherModel.Serialize(text, sizeof(text));
    g_launcherSavedEpoch = epoch;
    if (!bytes) { SerialPuts("WM: launcher layout exceeds 16 KiB; not saved\n"); return; }
    SpinLockAcquire(&g_launcherSaveLock);
    memcpy(g_launcherSaveText, text, bytes);
    g_launcherSaveBytes = bytes;
    g_launcherSavePending = true;
    g_launcherSaveChanged = ApicTickCount();
    SpinLockRelease(&g_launcherSaveLock);
}

// Launcher worker: debounced atomic write.
static void LauncherSaveLayout()
{
    static char text[LauncherLayoutLimit];
    uint32_t bytes = 0;
    SpinLockAcquire(&g_launcherSaveLock);
    const bool due = g_launcherSavePending && ApicTickCount() - g_launcherSaveChanged >= 1000;
    if (due) {
        memcpy(text, g_launcherSaveText, g_launcherSaveBytes);
        bytes = g_launcherSaveBytes;
        g_launcherSavePending = false;
    }
    SpinLockRelease(&g_launcherSaveLock);
    if (!due) return;
    Vnode* vn = VfsOpenMode(LauncherLayoutTemp, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, 0644);
    if (!vn) { SerialPuts("WM: launcher layout: cannot create the temporary file\n"); return; }
    uint64_t offset = 0;
    const int written = VfsWrite(vn, text, bytes, &offset);
    VfsChown(vn, DesktopUid, DesktopGid);
    VfsClose(vn);
    if (written != static_cast<int>(bytes) || VfsRename(LauncherLayoutTemp, LauncherLayoutPath) != 0)
        SerialPrintf("WM: launcher layout save failed (wrote %d of %u)\n", written, bytes);
    else SerialPrintf("WM: launcher layout saved (%u bytes)\n", bytes);
}

static ChromeLauncherChrome LauncherChrome(int32_t mouseX, int32_t mouseY)
{
    static char build[64];
    if (g_buildPayloadId[0]) WmStrCopy(build, g_buildPayloadId, sizeof(build));
    else {
        WmStrCopy(build, BuildGitHash(), sizeof(build));
        for (char* p = build; *p; ++p) if (*p == '+') { *p = 0; break; }
    }
    const bool shutdownActive = !ShutdownLaunchAllowed();
    const char* status = g_launcherError == -2 ? "App launch already pending" : g_launcherError ? "Launch queue full" : nullptr;
    const char* desktop = ShutdownDesktopStatus();
    if (!status && desktop && strcmp(desktop, "Applications")) status = desktop;
    return {g_launcherModelIcon, g_launcherModel.AppCount(), "brook", build, status,
            g_launcherPower.prompt(), g_launcherPower.pending() == LauncherPowerAction::Restart,
            g_launcherPower.confirmFocused(), shutdownActive, mouseX, mouseY};
}

// The frame for the current model, with the model's viewport in step.
static const ChromeLauncherFrame& LauncherFrame(uint32_t screenW, uint32_t screenH, int32_t mouseX, int32_t mouseY)
{
    const ChromeStyle style = WmChromeStyle();
    const ChromeLauncherChrome chrome = LauncherChrome(mouseX, mouseY);
    ChromeLauncherFrameFor(g_launcherModel, chrome, style, screenW, screenH, g_launcherFrame);
    g_launcherModel.SetViewport(g_launcherFrame.viewport);
    if (g_launcherModel.Scroll() != g_launcherFrame.scroll)
        ChromeLauncherFrameFor(g_launcherModel, chrome, style, screenW, screenH, g_launcherFrame);
    return g_launcherFrame;
}

static void LauncherLaunch(int app)
{
    if (app < 0 || app >= static_cast<int>(g_launcherModel.AppCount())) return;
    WmLauncherExec(g_launcherModelItem[app]);
}

static void LauncherRun(const LauncherResult& r)
{
    switch (r.command) {
    case LauncherCommand::None: return;
    case LauncherCommand::Changed: break;
    case LauncherCommand::Launch: LauncherLaunch(r.app); break;
    case LauncherCommand::Close: if (g_launcherOpen) WmLauncherToggle(); break;
    case LauncherCommand::OpenSettings: {
        int settings = -1;
        for (uint32_t i = 0; i < g_launcherModel.AppCount() && settings < 0; ++i)
            if (WmEqualI(g_launcherModel.App(i).name, "Settings")) settings = static_cast<int>(i);
        if (settings >= 0) { g_launcherModel.NoteLaunch(uint32_t(settings)); LauncherLaunch(settings); }
        break;
    }
    }
    LauncherNoteLayout();
    CompositorMarkDirty();
}

void WmLauncherScroll(int delta, uint32_t, uint32_t)
{
    if (!g_launcherOpen || !delta) return;
    // Pixel scrolling, a viewport step a tick (Ian, gen 458: two whole rows a
    // tick was too far and jumpy on the touchpad).
    if (g_launcherModel.ScrollTicks(delta)) CompositorMarkDirty();
}

void WmLauncherGetRect(uint32_t screenW, uint32_t screenH,
                       int32_t* outX, int32_t* outY,
                       uint32_t* outW, uint32_t* outH)
{
    const DesktopRect r = ChromeLauncherRect(WmChromeStyle(), screenW, screenH);
    *outX = r.x; *outY = r.y; *outW = static_cast<uint32_t>(r.w); *outH = static_cast<uint32_t>(r.h);
}

static void WmLauncherSurface(uint32_t* backBuffer, uint32_t stride,
                              uint32_t screenW, uint32_t screenH,
                              int32_t mouseX, int32_t mouseY, uint64_t keySeed, uint64_t* key)
{
    if (!g_launcherOpen || (!backBuffer && !key)) return;
    LauncherReadGuard guard; // the borrowed icons live in the active snapshot
    if (guard.get().generation != g_launcherModelGeneration) LauncherSyncModel(guard.get());
    const ChromeStyle style = WmChromeStyle();
    const ChromeLauncherFrame& frame = LauncherFrame(screenW, screenH, mouseX, mouseY);
    ChromeDisplayList list = ChromeListOn(g_shellListStorage);
    ChromeBuildLauncher(g_launcherModel, LauncherChrome(mouseX, mouseY), frame, style, list);
    if (key) *key = ChromeListKey(list, keySeed);
    else ChromeExecute(list, {backBuffer, stride, screenW, screenH}, WmChromeFonts());
}
void WmLauncherRender(uint32_t* backBuffer, uint32_t stride,
                      uint32_t screenW, uint32_t screenH,
                      int32_t mouseX, int32_t mouseY)
{
    WmLauncherSurface(backBuffer, stride, screenW, screenH, mouseX, mouseY, 0, nullptr);
}

int WmLauncherHitTest(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH)
{
    if (!g_launcherOpen) return -1;
    const DesktopRect r = ChromeLauncherRect(WmChromeStyle(), screenW, screenH);
    return r.contains(mx, my) ? 0 : -1;
}

// A pointer press while the launcher is open. False: outside the panel.
bool WmLauncherPress(int32_t mx, int32_t my, uint32_t screenW, uint32_t screenH, bool secondary)
{
    if (!g_launcherOpen) return false;
    LauncherReadGuard guard;
    if (guard.get().generation != g_launcherModelGeneration) LauncherSyncModel(guard.get());
    const ChromeLauncherFrame& frame = LauncherFrame(screenW, screenH, mx, my);
    const LauncherTarget target = ChromeLauncherHitTest(frame, mx, my);
    if (target.kind == LauncherTargetKind::None) return false;
    LauncherPowerTarget power = LauncherPowerTarget::None;
    switch (target.kind) {
    case LauncherTargetKind::Restart: power = LauncherPowerTarget::Restart; break;
    case LauncherTargetKind::Shutdown: power = LauncherPowerTarget::Shutdown; break;
    case LauncherTargetKind::Confirm: power = LauncherPowerTarget::Confirm; break;
    case LauncherTargetKind::Dismiss: power = LauncherPowerTarget::Dismiss; break;
    case LauncherTargetKind::CancelShutdown: power = LauncherPowerTarget::CancelShutdown; break;
    case LauncherTargetKind::Footer: power = LauncherPowerTarget::Footer; break;
    default: break;
    }
    if (power != LauncherPowerTarget::None) {
        if (secondary) return true;
        g_launcherModel.CloseMenu();
        LauncherPowerRun(g_launcherPower.Activate(power, ApicTickCount()));
        return true;
    }
    g_launcherPower.Reset(); // any other launcher press dismisses the prompt
    LauncherRun(g_launcherModel.Press(target, secondary, mx, my));
    CompositorMarkDirty();
    return true;
}

// A key press while the launcher is open (the power prompt's own keys go to
// WmLauncherPowerKey first).
void WmLauncherKey(uint8_t scan, char ascii, uint8_t modifiers)
{
    if (!g_launcherOpen) return;
    uint8_t mods = 0;
    if (modifiers & INPUT_MOD_ALT) mods |= LauncherModAlt;
    if (modifiers & INPUT_MOD_CTRL) mods |= LauncherModCtrl;
    if (modifiers & INPUT_MOD_SHIFT) mods |= LauncherModShift;
    LauncherKey key;
    switch (scan) {
    case 0x01: key = LauncherKey::Escape; break;
    case 0x0E: key = LauncherKey::Backspace; break;
    case 0x0F: key = mods & LauncherModShift ? LauncherKey::BackTab : LauncherKey::Tab; break;
    case 0x1C: key = LauncherKey::Enter; break;
    case SC_EXT_UP: key = LauncherKey::Up; break;
    case SC_EXT_DOWN: key = LauncherKey::Down; break;
    case SC_EXT_LEFT: key = LauncherKey::Left; break;
    case SC_EXT_RIGHT: key = LauncherKey::Right; break;
    case SC_EXT_HOME: key = LauncherKey::Home; break;
    case SC_EXT_END: key = LauncherKey::End; break;
    case SC_EXT_PGUP: key = LauncherKey::PageUp; break;
    case SC_EXT_PGDN: key = LauncherKey::PageDown; break;
    default:
        // Ctrl and Alt chords are not text.
        if (!ascii || (mods & (LauncherModCtrl | LauncherModAlt))) return;
        key = LauncherKey::Char;
        break;
    }
    LauncherReadGuard guard;
    if (guard.get().generation != g_launcherModelGeneration) LauncherSyncModel(guard.get());
    if (g_launcherPower.prompt() && key != LauncherKey::Escape) g_launcherPower.Reset();
    LauncherRun(g_launcherModel.Key(key, ascii, mods));
}

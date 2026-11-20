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

// The compositor queues serialized layouts. Only the worker writes them.
static constexpr const char* LauncherLayoutPath = "/data/home/.config/brook/launcher.ini";
static constexpr const char* LauncherLayoutName = "launcher.ini";
static constexpr const char* LauncherLayoutTemp = "launcher.ini.tmp";
static constexpr uint32_t LauncherLayoutLimit = 16 * 1024;
static SpinLock g_launcherSaveLock;
static LauncherPersistenceStatus g_launcherPersistence;
static char g_launcherSaveText[LauncherLayoutLimit];
static uint32_t g_launcherSaveBytes = 0;
static uint32_t g_launcherQueuedEpoch = 0;
static uint64_t g_launcherSaveSince = 0;
static uint32_t g_launcherSaveDelay = 0;

static void LauncherNoteLayout(bool firstCatalog = false);

void WmLauncherPersistenceStatus(LauncherPersistenceStatus& out)
{
    SpinLockAcquire(&g_launcherSaveLock);
    out = g_launcherPersistence;
    SpinLockRelease(&g_launcherSaveLock);
    const bool running = __atomic_load_n(&g_launcherWorkerRunning, __ATOMIC_ACQUIRE);
    const bool started = __atomic_load_n(&g_launcherWorkerStarted, __ATOMIC_ACQUIRE);
    out.worker = running ? "running" : started ? "starting" : "stopped";
    out.path = LauncherLayoutPath;
}

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
    const bool firstCatalog = g_launcherModelGeneration == 0;
    g_launcherModel.SetCatalog(apps, count);
    g_launcherModelGeneration = catalog.generation;
    LauncherNoteLayout(firstCatalog);
}

// Read a regular config file of under `limit` bytes into text (terminated).
// Shell context: the VFS may sleep.
static LauncherPersistenceResult ConfigReadText(const char* configPath, char* text, uint32_t limit)
{
    VfsPath root{};
    int error = VfsPathRoot(&root);
    if (error) return {"root", error};
    VfsPath path{};
    error = VfsPathResolve(root, root, configPath, &path, false);
    VfsPathRelease(root);
    if (error) return {error == -2 ? "missing" : "lookup", error};
    VnodeStat stat{};
    error = VfsPathStat(path, &stat);
    if (error || (stat.mode & 0170000) != 0100000) {
        VfsPathRelease(path);
        return {"type", error ? error : -22};
    }
    auto* file = VfsPathOpen(path, VFS_O_READ, &error);
    VfsPathRelease(path);
    if (!file) return {"open", error};
    uint32_t done = 0;
    uint64_t offset = 0;
    while (!error && done < limit) {
        const int rd = VfsRead(file, text + done, limit - done, &offset);
        if (rd <= 0) { error = rd; break; }
        done += static_cast<uint32_t>(rd);
    }
    VfsClose(file);
    if (error) return {"read", error};
    if (done == limit) return {"size", -7};
    text[done] = 0;
    return {"restored", 0};
}

static LauncherPersistenceResult LauncherReadLayout()
{
    auto* text = static_cast<char*>(kmalloc(LauncherLayoutLimit));
    if (!text) return {"read", -12};
    LauncherPersistenceResult result = ConfigReadText(LauncherLayoutPath, text, LauncherLayoutLimit);
    if (!result.error) {
        bool migrated = false;
        if (!g_launcherModel.Parse(text, &migrated)) result = {"parse", -22};
        else if (migrated) result = {"migrated", 0};
    }
    kfree(text);
    return result;
}

static void LauncherLoadLayout()
{
    g_launcherModel.Init();
    const auto result = LauncherReadLayout();
    SpinLockAcquire(&g_launcherSaveLock);
    g_launcherPersistence.load = result;
    if (!result.error && !strcmp(result.stage, "restored")) g_launcherPersistence.savedEpoch = g_launcherModel.LayoutEpoch();
    SpinLockRelease(&g_launcherSaveLock);
    if (!result.error) KLOG_INFO(Wm, "launcher: layout %s", result.stage);
    else if (strcmp(result.stage, "missing"))
        KLogCatConsole(LogCat::Wm, LogLevel::Warn, "launcher: load failed stage=%s errno=%d", result.stage, -result.error);
}

// The model is owned by the compositor after initial activation. Publish a
// snapshot of the stable keys and serialized bytes for the other readers.
static void LauncherNoteLayout(bool firstCatalog)
{
    const uint32_t epoch = g_launcherModel.LayoutEpoch();
    static char text[LauncherLayoutLimit];
    SpinLockAcquire(&g_launcherSaveLock);
    auto& state = g_launcherPersistence;
    if (epoch == state.layoutEpoch) {
        SpinLockRelease(&g_launcherSaveLock);
        return;
    }
    state.layoutEpoch = epoch;
    state.pinCount = g_launcherModel.PinCount();
    for (uint32_t i = 0; i < state.pinCount; ++i)
        WmStrCopy(state.pins[i], g_launcherModel.PinKey(i), sizeof(state.pins[i]));

    // Write defaults or the upgraded layout at first boot. Preserve an unreadable or invalid
    // existing file until the user actually changes the layout.
    const bool seed = !strcmp(state.load.stage, "missing") || !strcmp(state.load.stage, "migrated");
    if (firstCatalog && !seed) {
        SpinLockRelease(&g_launcherSaveLock);
        return;
    }
    SpinLockRelease(&g_launcherSaveLock);
    const uint32_t bytes = g_launcherModel.Serialize(text, sizeof(text));
    SpinLockAcquire(&g_launcherSaveLock);
    if (bytes) memcpy(g_launcherSaveText, text, bytes);
    g_launcherSaveBytes = bytes;
    g_launcherQueuedEpoch = epoch;
    state.pending = g_launcherSaveBytes != 0;
    g_launcherSaveSince = ApicTickCount();
    g_launcherSaveDelay = 1000;
    if (!state.pending) state.save = {"serialize", -7};
    const bool queued = state.pending;
    SpinLockRelease(&g_launcherSaveLock);
    if (!queued) KLogCatConsole(LogCat::Wm, LogLevel::Warn, "launcher: save failed stage=serialize errno=7");
}

// Open config directories relative to the actual home attachment. Create
// only the two launcher config ancestors, leaving existing ownership alone.
static int LauncherConfigDir(const VfsPath& root, VfsPath* out)
{
    VfsPath directory{};
    int error = VfsPathResolve(root, root, "/data/home", &directory);
    const char* parts[] = {".config", "brook"};
    for (const char* part : parts) {
        if (error) break;
        VfsPath next{};
        error = VfsPathResolve(root, directory, part, &next, false);
        if (error == -2) {
            error = VfsMkdirAt(root, directory, part, 0700, 0);
            if (!error) error = VfsChownAt(root, directory, part, DesktopUid, DesktopGid, false);
            if (!error || error == -17) error = VfsPathResolve(root, directory, part, &next, false);
        }
        VfsPathRelease(directory);
        directory = next;
    }
    if (error) VfsPathRelease(directory);
    else *out = directory;
    return error;
}

static LauncherPersistenceResult LauncherWriteLayout(const VfsPath& root, const VfsPath& directory,
                                                     const char* text, uint32_t bytes,
                                                     const char* temp = LauncherLayoutTemp,
                                                     const char* name = LauncherLayoutName)
{
    // A power cut can leave our temporary entry behind. Unlink the entry
    // itself, then create exclusively so we never truncate an existing inode.
    int error = VfsRemoveAt(root, directory, temp, false);
    if (error && error != -2) return {"remove-temp", error};
    auto* file = VfsOpenAt(root, directory, temp,
                           VFS_O_WRITE | VFS_O_CREATE | VFS_O_EXCL, 0600, 0, &error, false);
    if (!file) return {"create", error};

    LauncherPersistenceResult result{"saved", 0};
    uint64_t offset = 0;
    while (offset < bytes) {
        const int written = VfsWrite(file, text + offset, bytes - offset, &offset);
        if (written <= 0) { result = {"write", written ? written : -5}; break; }
    }
    if (!result.error && (error = VfsChown(file, DesktopUid, DesktopGid))) result = {"chown", error};
    if (!result.error && (error = VfsFsync(file))) result = {"sync-file", error};
    VfsClose(file);
    if (!result.error && (error = VfsRenameAt(root, directory, temp, directory, name)))
        result = {"rename", error};
    if (result.error) {
        VfsRemoveAt(root, directory, temp, false);
        return result;
    }
    auto* dir = VfsPathOpen(directory, VFS_O_READ, &error);
    if (!dir) return {"open-directory", error};
    error = VfsFsync(dir);
    VfsClose(dir);
    return error ? LauncherPersistenceResult{"sync-directory", error} : result;
}

static void LauncherSaveLayout()
{
    static char text[LauncherLayoutLimit];
    SpinLockAcquire(&g_launcherSaveLock);
    auto& state = g_launcherPersistence;
    if (!state.pending || ApicTickCount() - g_launcherSaveSince < g_launcherSaveDelay) {
        SpinLockRelease(&g_launcherSaveLock);
        return;
    }
    const uint32_t epoch = g_launcherQueuedEpoch, bytes = g_launcherSaveBytes;
    memcpy(text, g_launcherSaveText, bytes);
    state.saving = true;
    SpinLockRelease(&g_launcherSaveLock);

    VfsPath root{}, directory{};
    LauncherPersistenceResult result;
    if (const int error = VfsPathRoot(&root)) result = {"root", error};
    else if (const int dirError = LauncherConfigDir(root, &directory)) result = {"directory", dirError};
    else result = LauncherWriteLayout(root, directory, text, bytes);
    VfsPathRelease(directory);
    VfsPathRelease(root);

    SpinLockAcquire(&g_launcherSaveLock);
    const bool newResult = result.error != state.save.error || strcmp(result.stage, state.save.stage);
    state.save = result;
    state.saving = false;
    if (!result.error) state.savedEpoch = epoch;
    if (g_launcherQueuedEpoch == epoch) {
        state.pending = result.error != 0;
        g_launcherSaveSince = ApicTickCount();
        g_launcherSaveDelay = 5000;
    }
    SpinLockRelease(&g_launcherSaveLock);
    if (result.error && newResult)
        KLogCatConsole(LogCat::Wm, LogLevel::Warn, "launcher: save failed stage=%s errno=%d", result.stage, -result.error);
    if (!result.error) KLOG_INFO(Wm, "launcher: layout saved epoch=%u bytes=%u", epoch, bytes);
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
            g_launcherPower.confirmFocused(), shutdownActive, mouseX, mouseY, ShutdownCanForceApps()};
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

void WmLauncherScroll(int delta120, uint32_t, uint32_t)
{
    if (!g_launcherOpen || !delta120) return;
    // Pixel scrolling in proportion to the motion: a viewport step a wheel
    // tick, and touchpad motion finer than a tick (Ian, gen 458/459: whole
    // rows, then whole ticks, were jumpy; it should follow scroll speed).
    static int remainder = 0;
    const int step = g_launcherFrame.viewport.step > 0 ? g_launcherFrame.viewport.step : 20;
    const int total = delta120 * step + remainder;
    const int pixels = total / 120;
    remainder = total - pixels * 120;
    if (pixels && g_launcherModel.ScrollBy(-pixels)) CompositorMarkDirty();
}

void WmLauncherGetRect(uint32_t screenW, uint32_t screenH,
                       int32_t* outX, int32_t* outY,
                       uint32_t* outW, uint32_t* outH)
{
    const DesktopRect r = ChromeLauncherRect(WmChromeStyle(), screenW, screenH);
    *outX = r.x; *outY = r.y; *outW = static_cast<uint32_t>(r.w); *outH = static_cast<uint32_t>(r.h);
}

static void WmCaptureLauncher(uint32_t screenW, uint32_t screenH, int32_t mouseX, int32_t mouseY,
                              ChromeShellModel& snapshot)
{
    if (!g_launcherLoaded) return;
    LauncherReadGuard guard;
    if (guard.get().generation != g_launcherModelGeneration) LauncherSyncModel(guard.get());
    const ChromeStyle style = WmChromeStyle();
    const DesktopRect bounds = ChromeLauncherRect(style, screenW, screenH);
    const bool hovered = g_launcherOpen && bounds.contains(mouseX, mouseY);
    const int32_t mx = hovered ? mouseX : -1, my = hovered ? mouseY : -1;
    auto chrome = LauncherChrome(mx, my);

    snapshot.kind = ChromeShellKind::Launcher;
    snapshot.launcherModel = g_launcherModel;
    if (g_launcherOpen) {
        snapshot.launcherFrame = LauncherFrame(screenW, screenH, mx, my);
        snapshot.launcherModel = g_launcherModel; // captures the clamped viewport
    } else {
        // Prime the same default view WmLauncherToggle will open.
        chrome.prompt = chrome.promptRestart = chrome.confirmFocused = false;
        if (g_launcherError) {
            const char* desktop = ShutdownDesktopStatus();
            chrome.status = desktop && strcmp(desktop, "Applications") ? desktop : nullptr;
        }
        snapshot.launcherModel.Open();
        ChromeLauncherFrameFor(snapshot.launcherModel, chrome, style, screenW, screenH, snapshot.launcherFrame);
        snapshot.launcherModel.SetViewport(snapshot.launcherFrame.viewport);
    }
    snapshot.mouseX = mx;
    snapshot.mouseY = my;
    snapshot.shutdownActive = chrome.shutdownActive;
    snapshot.forceApps = chrome.forceApps;
    snapshot.prompt = chrome.prompt;
    snapshot.promptRestart = chrome.promptRestart;
    snapshot.confirmFocused = chrome.confirmFocused;
    if (chrome.status) WmStrCopy(snapshot.text, chrome.status, sizeof(snapshot.text));
    if (chrome.build) WmStrCopy(snapshot.launcherBuild, chrome.build, sizeof(snapshot.launcherBuild));
    for (uint32_t i = 0; i < chrome.iconCount; ++i)
        WmCaptureShellIcon(snapshot.icons[i], chrome.icons[i]);
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
    case LauncherTargetKind::ForceApps: power = LauncherPowerTarget::ForceApps; break;
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

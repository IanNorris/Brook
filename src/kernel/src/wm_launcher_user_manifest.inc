// Read by the initial shell context or launcher worker. Resolve and read an
// exact regular inode so an atomic writer rename cannot switch the file
// between ownership/size validation and open.
static uint32_t* LauncherLoadUserIcon(const VfsPath& root, const VfsPath& icons, const char* name)
{
    if (!icons.object || !*name) return nullptr;
    VfsPath path{};
    if (VfsPathResolve(root, icons, name, &path, false)) return nullptr;
    VnodeStat st{};
    int error = 0;
    Vnode* node = nullptr;
    if (!VfsPathStat(path, &st) && (st.mode & 0170000) == 0100000 &&
        st.uid == DesktopUid && st.size == LAUNCHER_ICON_BYTES)
        node = VfsPathOpen(path, VFS_O_READ, &error);
    VfsPathRelease(path);
    if (!node) return nullptr;

    auto* pixels = static_cast<uint32_t*>(kmalloc(LAUNCHER_ICON_BYTES));
    uint64_t offset = 0;
    if (pixels) {
        while (offset < LAUNCHER_ICON_BYTES) {
            const int read = VfsRead(node, reinterpret_cast<uint8_t*>(pixels) + offset,
                                     LAUNCHER_ICON_BYTES - offset, &offset);
            if (read <= 0) break;
        }
        if (offset != LAUNCHER_ICON_BYTES) {
            kfree(pixels);
            pixels = nullptr;
        }
    }
    VfsClose(node);
    return pixels;
}

static LauncherPersistenceResult LauncherReadUserManifest(const VfsPath& root, char* bytes, uint32_t& size)
{
    VfsPath path{};
    int error = VfsPathResolve(root, root, UserLauncherManifestPath, &path, false);
    if (error) return {error == -2 ? "missing" : "lookup", error};

    VnodeStat st{};
    error = VfsPathStat(path, &st);
    LauncherPersistenceResult result{"loaded", 0};
    Vnode* node = nullptr;
    if (error) result = {"stat", error};
    else if (!UserLauncherManifestMode(st.mode, st.uid)) result = {"owner-or-mode", -13};
    else if (st.size > UserLauncherManifestLimit) result = {"size", -7};
    else {
        node = VfsPathOpen(path, VFS_O_READ, &error);
        if (!node) result = {"open", error};
    }
    VfsPathRelease(path);
    if (!node) return result;

    uint64_t offset = 0;
    while (offset <= UserLauncherManifestLimit) {
        const int read = VfsRead(node, bytes + offset, UserLauncherManifestLimit + 1 - offset, &offset);
        if (read < 0) { result = {"read", read}; break; }
        if (read == 0) break;
    }
    VfsClose(node);
    if (offset > UserLauncherManifestLimit) return {"size", -7};
    if (!result.error && offset != st.size) return {"changed-during-read", -11};
    size = static_cast<uint32_t>(offset);
    return result;
}

static const char* LauncherAddUserEntry(LauncherSnapshot& catalog, const VfsPath& root,
                                        const VfsPath& icons, const UserLauncherEntry& entry)
{
    if (catalog.count == WM_LAUNCHER_MAX_ITEMS) return "catalog limit";
    for (uint32_t i = 0; i < catalog.count; ++i) {
        const auto& item = catalog.items[i];
        if (UserLauncherConflicts(entry, item.title, item.appId)) return "duplicate ID or title";
    }
    LauncherItem item{};
    static_assert(sizeof(item.scriptPath) == UserLauncherCommandCapacity);
    if (!LauncherUserExecutable(entry.command, item.executable)) return "profile executable unavailable";
    WmStrCopy(item.title, entry.title, sizeof(item.title));
    WmStrCopy(item.appId, entry.appId, sizeof(item.appId));
    WmStrCopy(item.scriptPath, entry.command, sizeof(item.scriptPath));
    WmStrCopy(item.category, LauncherModel::HintFromDesktopCategories(entry.categories), sizeof(item.category));
    item.valid = item.isDesktopEntry = item.isUserDesktopEntry = true;
    item.isWebApp = HasWebAppCategory(entry.categories);
    if (!IdentifyLauncherItem(catalog, catalog.count, item)) return "catalog identity conflict";
    item.iconPixels = LauncherLoadUserIcon(root, icons, entry.icon);
    catalog.items[catalog.count++] = item;
    return nullptr;
}

static void LauncherLoadUserManifest(LauncherSnapshot& catalog)
{
    LauncherUserManifestStatus status{};
    VfsPath root{};
    int error = VfsPathRoot(&root);
    if (error) status.load = {"root", error};
    else {
        auto* bytes = static_cast<char*>(kmalloc(UserLauncherManifestLimit + 1));
        if (!bytes) status.load = {"allocate", -12};
        else {
            uint32_t size = 0;
            status.load = LauncherReadUserManifest(root, bytes, size);
            if (!status.load.error && size) {
                VfsPath icons{};
                VfsPathResolve(root, root, UserLauncherIconDirectory, &icons, false);
                constexpr uint32_t LogLimit = 16;
                uint32_t logged = 0;
                const auto counts = LoadUserLauncherRecords(bytes, size,
                    [&](const UserLauncherEntry& entry) {
                        return LauncherAddUserEntry(catalog, root, icons, entry);
                    },
                    [&](uint32_t line, const char* reason) {
                        if (logged == LogLimit) return;
                        ++logged;
                        KLogCatConsole(LogCat::Wm, LogLevel::Warn,
                            "launcher: user manifest line %u rejected (%s)", line, reason);
                    });
                status.entries = counts.entries;
                status.rejected = counts.rejected;
                if (status.rejected > logged)
                    KLogCatConsole(LogCat::Wm, LogLevel::Warn,
                        "launcher: %u additional user manifest rejections", status.rejected - logged);
                VfsPathRelease(icons);
            }
            kfree(bytes);
        }
        VfsPathRelease(root);
    }
    if (status.load.error && strcmp(status.load.stage, "missing"))
        KLogCatConsole(LogCat::Wm, LogLevel::Warn, "launcher: user manifest %s errno=%d",
                       status.load.stage, -status.load.error);
    SpinLockAcquire(&g_launcherLock);
    g_userLauncherManifest = status;
    SpinLockRelease(&g_launcherLock);
}

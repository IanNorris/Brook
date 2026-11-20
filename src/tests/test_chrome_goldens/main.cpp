// Chrome golden images (compositor refactor stage 1.2): every chrome element
// is built as a display list and rasterised by the freestanding CPU executor,
// per theme and scale, then compared byte for byte with the PNG under
// src/tests/goldens/chrome/. The ARGB buffer (alpha included) is also pinned
// by hash in MANIFEST. This binary links only the freestanding chrome
// sources and the baked chrome fonts and icon masks: no kernel code.
//
//   test_chrome_goldens            compare; on a mismatch, write the actual
//                                  image to ./chrome-golden-actual/ and fail
//   test_chrome_goldens --update   rewrite the goldens (review the diff)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "chrome_frame.h"
#include "chrome_shell_model.h"
#include "chrome_shell_wire.h"
#include "web_app_title.h"
#include "png.h"

using namespace brook;

#ifndef BROOK_CHROME_GOLDENS
#error "BROOK_CHROME_GOLDENS must name the golden directory"
#endif

namespace {

bool update = false;
int failures = 0, written = 0, compared = 0;
std::map<std::string, std::string> manifest, oldManifest;
const std::string goldenDir = BROOK_CHROME_GOLDENS;

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buffer[65536];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) data.insert(data.end(), buffer, buffer + n);
        std::fclose(f);
    }
    return data;
}
void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); std::exit(2); }
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

std::map<uint64_t, uint64_t> listKeys; // display list key -> pixel hash
uint64_t Fnv(const uint32_t* pixels, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        for (int s = 0; s < 32; s += 8) { h ^= (pixels[i] >> s) & 255; h *= 1099511628211ull; }
    return h;
}

// Rasterise the list into a w x h image whose top-left is (ox, oy) in list
// coordinates, over a uniform legacy-RGB backdrop (alpha 0, as the back
// buffer), and check it against the golden.
void Check(const std::string& name, const ChromeDisplayList& list, const ChromeStyle& style,
           int ox, int oy, int w, int h) {
    if (list.Overflowed()) { std::fprintf(stderr, "%s: display list overflowed\n", name.c_str()); ++failures; }
    if (w <= 0 || h <= 0) { std::fprintf(stderr, "%s: empty image\n", name.c_str()); ++failures; return; }
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0x00303030u);
    ChromeTarget target{pixels.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(w), static_cast<uint32_t>(h), ox, oy};
    ChromeExecute(list, target, *style.fonts);
    // Content keys (stage 1.4): equal keys must mean equal pixels. The kernel
    // seeds the key with the font and the surface rectangle, as here.
    {
        const uint64_t seed = reinterpret_cast<uintptr_t>(style.fonts) ^ (uint64_t(uint32_t(ox)) << 32 | uint32_t(oy)) ^
                              (uint64_t(uint32_t(w)) << 48) ^ (uint64_t(uint32_t(h)) << 16);
        const uint64_t key = ChromeListKey(list, seed), pixelsKey = Fnv(pixels.data(), pixels.size());
        if (!key || ChromeListKey(list, seed) != key) { std::fprintf(stderr, "%s: unstable or zero key\n", name.c_str()); ++failures; }
        const auto seen = listKeys.find(key);
        if (seen != listKeys.end() && seen->second != pixelsKey) {
            std::fprintf(stderr, "%s: content key collides with an image of different pixels\n", name.c_str()); ++failures;
        }
        listKeys[key] = pixelsKey;
    }
    const std::vector<uint8_t> png = chrome_png::EncodeRgb(pixels.data(), w, h);
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(Fnv(pixels.data(), pixels.size())));
    manifest[name] = hash;
    const std::string path = goldenDir + "/" + name + ".png";
    if (update) { WriteFile(path, png); ++written; return; }
    ++compared;
    const std::vector<uint8_t> golden = ReadFile(path);
    const auto old = oldManifest.find(name);
    const bool pngSame = golden == png, alphaSame = old != oldManifest.end() && old->second == hash;
    if (pngSame && alphaSame) return;
    mkdir("chrome-golden-actual", 0755);
    WriteFile("chrome-golden-actual/" + name + ".png", png);
    std::fprintf(stderr, "%s: %s differs from %s (actual in chrome-golden-actual/)\n", name.c_str(),
                 golden.empty() ? "missing golden" : !pngSame ? "RGB" : "ARGB hash", path.c_str());
    ++failures;
}

// A deterministic 24x24 straight-alpha icon: a disc with a diagonal ramp.
uint32_t testIcon[24 * 24];
void MakeIcon() {
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const int dx = 2 * x - 23, dy = 2 * y - 23, r2 = dx * dx + dy * dy;
            const uint32_t a = r2 <= 400 ? 255 : r2 <= 484 ? 128 : 0;
            testIcon[y * 24 + x] = a << 24 | static_cast<uint32_t>(40 + 8 * x) << 16 |
                                   static_cast<uint32_t>(200 - 6 * y) << 8 | 0x60u;
        }
}

ChromeMaterialCache glassCache;

// The renderer captures explicit alpha for Glass and solid pixels for Opaque.
struct GoldenTheme { const char* name; unsigned material, mode, hue; uint32_t rgb = 0; };
BrookThemePalette currentPalette;
void SetPalette(const GoldenTheme& t) {
    BrookHueTokens tokens;
    brook_hue_derive_selection(brook_selection_make(t.material,t.mode,t.hue,t.rgb), &tokens);
    brook_theme_build_palette(&tokens, t.material, brook_theme_chrome_dark(t.mode), &currentPalette);
}

ChromeStyle StyleFor(uint32_t, const DesktopMetrics& m, bool glass) {
    static const ChromeFontSet* fonts[] = {&g_chromeFonts100, &g_chromeFonts150, &g_chromeFonts200};
    const ChromeFontSet* font = fonts[m.percent == 100 ? 0 : m.percent == 150 ? 1 : 2];
    const BrookThemePalette* p = &currentPalette;
    ChromeStyle s{*p, &m, font, glass, glass ? 200u : 255u, nullptr};
    if (glass) {
        glassCache.Ensure(p->titleActive, p->titleInactive, 200, s.M(DesktopMetrics::TitleBase), s.M(8),
                          s.M(2), p, s.M(DesktopMetrics::TaskbarBase));
        s.material = &glassCache;
    }
    return s;
}

// --- Window frames ----------------------------------------------------------

void Frames(const std::string& prefix, uint32_t theme, const DesktopMetrics& m) {
    const int clientW = m.pixels(360), clientH = m.pixels(80);
    struct Case { const char* name; bool focused, maximised, glass; const uint32_t* icon; int hover; uint8_t dock = 0; };
    // dock: the side-dock caption button (0 none, 1 left, 2 right).
    // hover: -1 none; 0..3 a caption button by its outer top-right corner of
    // its hit rect (the bleed area, not the visual cell).
    const Case cases[] = {
        {"frame-active", true, false, false, testIcon, -1},
        {"frame-inactive", false, false, false, nullptr, -1},
        {"frame-hover-close", true, false, false, testIcon, 0},
        {"frame-hover-max", true, false, false, testIcon, 1},
        {"frame-hover-min", false, false, false, testIcon, 2},
        {"frame-maximised", true, true, false, testIcon, -1},
        {"frame-active-glass", true, false, true, testIcon, -1},
        {"frame-dock-hover", true, false, false, testIcon, 3, 1},
        {"frame-dock-right-glass", true, false, true, testIcon, -1, 2},
    };
    for (const Case& c : cases) {
        const ChromeStyle s = StyleFor(theme, m, c.glass);
        ChromeFrameModel model{};
        DesktopWindowGeometry g{0, 0, clientW, clientH, false};
        g.dockButton = c.dock;
        model.layout = ChromeFrameLayout({g, c.maximised, {}}, m);
        model.clientW = clientW;
        model.focused = c.focused;
        model.maximised = c.maximised;
        model.title = "Terminal - brook@clearwater: ~/src";
        model.icon = c.icon;
        model.mouseX = model.mouseY = -1;
        if (c.hover >= 0) {
            const ChromePart* part = model.layout.Find(ChromeCaptionRole(c.hover));
            model.mouseX = part->hit.x + part->hit.w - 1;
            model.mouseY = part->hit.y;
        }
        ChromeFrameStorage storage;
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildFrame(model, s, list);
        const DesktopRect o = model.layout.outer;
        Check(prefix + c.name, list, s, o.x, o.y, o.w, o.h);
    }
    // Client-decorated windows with overlaid captions (chrome-csd): native
    // title style, and matched to a dark client strip with a rounded corner.
    const ChromeStyle s = StyleFor(theme, m, false);
    for (int styled = 0; styled < 2; ++styled) {
        ChromeFrameModel model{};
        const DesktopWindowGeometry g{0, 0, clientW, clientH, true, true, 0, 0, 0};
        model.layout = ChromeFrameLayout({g, false, {}}, m);
        model.clientW = clientW;
        model.focused = true;
        model.clientDecorated = true;
        model.captionStyle = styled ? DesktopCaptionStyleValid | 0x202124u | (8u << 25) : 0;
        model.title = "";
        const ChromePart* close = model.layout.Find(ChromePartRole::Close);
        model.mouseX = styled ? close->hit.x + 1 : -1;
        model.mouseY = styled ? close->hit.y + 1 : -1;
        ChromeFrameStorage storage;
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildFrame(model, s, list);
        Check(prefix + (styled ? "frame-csd-styled" : "frame-csd"), list, s, 0, 0, clientW, clientH / 2);
    }
}

// --- Launcher ------------------------------------------------------------------

// The prototype's sample catalog, filed by the default categories; a few
// apps have no icon (the neutral missing-icon glyph) or no description.
LauncherApp LauncherSample(const char* key, const char* name, const char* desc, const char* hint) {
    LauncherApp a{};
    std::snprintf(a.key, sizeof(a.key), "%s", key);
    std::snprintf(a.name, sizeof(a.name), "%s", name);
    std::snprintf(a.desc, sizeof(a.desc), "%s", desc);
    std::snprintf(a.hint, sizeof(a.hint), "%s", hint);
    return a;
}
const LauncherApp launcherApps[] = {
    LauncherSample(LauncherFilesAppId, "Files", "Browse files and directories", "system"),
    LauncherSample("terminal", "Terminal", "Command line shell", "system"),
    LauncherSample("brook-settings", "Settings", "Appearance and taskbar", "system"),
    LauncherSample("kconsole", "Kernel Console", "Read-only kernel log", "system"),
    LauncherSample("gallery", "App Gallery", "Find and install applications", "system"),
    LauncherSample("firefox", "Firefox", "Web browser", "internet"),
    LauncherSample("ladybird", "Ladybird", "Ladybird browser", "internet"),
    LauncherSample("netsurf", "NetSurf", "Lightweight web browser", "internet"),
    LauncherSample("player", "Video Player", "Brook video player", "media"),
    LauncherSample("vlc", "VLC media player", "Plays most media files", "media"),
    LauncherSample("edit", "Text Editor", "Open, edit, save", "office"),
    LauncherSample("mousepad", "Mousepad", "", "office"),
    LauncherSample("calc", "Calculator", "Brook native calculator", "office"),
    LauncherSample("gimp", "GIMP", "Image manipulation", "graphics"),
    LauncherSample("doom", "DOOM", "The classic", "games"),
    LauncherSample("quake", "Quake", "id Software, 1996", "games"),
    LauncherSample("quake2", "Quake II", "id Software, 1997", "games"),
    LauncherSample("flower", "Flower Demo", "Weston animated flower", "demos"),
    LauncherSample("htop", "htop", "Process viewer", ""),
};
constexpr uint32_t launcherAppCount = sizeof(launcherApps) / sizeof(launcherApps[0]);

void RenderSnapshot(const ChromeShellModel&, const ChromeStyle&, ChromeDisplayList&);
void SetTestIcon(ChromeShellIcon&);

void Launcher(const std::string& prefix, const ChromeStyle& s, uint32_t screenW, uint32_t screenH) {
    static LauncherModel model;
    static ChromeLauncherFrame frame;
    static ChromeShellStorage storage;
    const uint32_t* icons[launcherAppCount];
    for (uint32_t i = 0; i < launcherAppCount; ++i) icons[i] = i % 5 == 3 ? nullptr : testIcon;
    struct Scene { const char* name; void (*stage)(LauncherModel&, const ChromeLauncherFrame&, ChromeLauncherChrome&); };
    auto rowOf = [](const ChromeLauncherFrame& f, int app) {
        for (uint32_t i = 0; i < f.targetCount; ++i)
            if (f.targets[i].target.kind == LauncherTargetKind::Row && f.targets[i].target.index == app) return f.targets[i].rect;
        return DesktopRect{};
    };
    (void)rowOf;
    static const Scene scenes[] = {
        // Opened: empty search, pinned tiles, All apps.
        {"launcher", [](LauncherModel&, const ChromeLauncherFrame&, ChromeLauncherChrome&) {}},
        // Typed "ga": the Games category chip and ranked results with category chips.
        {"launcher-search", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            m.Key(LauncherKey::Char, 'g'); m.Key(LauncherKey::Char, 'a'); }},
        // Nothing matches.
        {"launcher-nomatch", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            for (const char* q = "zork"; *q; ++q) m.Key(LauncherKey::Char, *q); }},
        // The pointer on Ladybird.
        {"launcher-hover", [](LauncherModel& m, const ChromeLauncherFrame& f, ChromeLauncherChrome& c) {
            for (uint32_t i = 0; i < f.targetCount; ++i)
                if (f.targets[i].target.kind == LauncherTargetKind::Row && f.targets[i].target.index == m.FindApp("ladybird")) {
                    c.mouseX = f.targets[i].rect.x + 5; c.mouseY = f.targets[i].rect.y + 5;
                } }},
        // Keyboard: four Downs in the list, and Recent on top.
        {"launcher-keyboard", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            m.NoteLaunch(m.FindApp("quake2")); m.NoteLaunch(m.FindApp("edit"));
            m.SetTop(LauncherTop::Recent);
            for (int i = 0; i < 4; ++i) m.Key(LauncherKey::Down); }},
        // Right-clicked Ladybird: the Pin to top / Uninstall menu.
        {"launcher-menu", [](LauncherModel& m, const ChromeLauncherFrame& f, ChromeLauncherChrome& c) {
            for (uint32_t i = 0; i < f.targetCount; ++i)
                if (f.targets[i].target.kind == LauncherTargetKind::Row && f.targets[i].target.index == m.FindApp("ladybird")) {
                    const DesktopRect r = f.targets[i].rect;
                    m.Press(f.targets[i].target, true, r.x + r.w * 45 / 100, r.y + r.h * 6 / 10);
                    c.mouseX = f.targets[i].rect.x + 5; c.mouseY = f.targets[i].rect.y + 5;
                } }},
        // A category view, scrolled part way into a row (the list clips it),
        // with nothing pinned.
        {"launcher-category", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            for (const char* key : {LauncherFilesAppId, "terminal", "brook-settings", "firefox", "quake"}) m.TogglePin(m.FindApp(key));
            m.Press({LauncherTargetKind::Rail, 0}, false, 0, 0); m.ScrollTicks(-5); }},
        {"launcher-organise", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            using TK = LauncherTargetKind;
            m.Press({TK::Organise, 0}, false, 0, 0);
            m.Press({TK::Row, uint16_t(m.FindApp("ladybird"))}, false, 0, 0);
            m.Press({TK::Row, uint16_t(m.FindApp("firefox"))}, false, 0, 0);
            m.ScrollBy(160); }},
        {"launcher-drag", [](LauncherModel& m, const ChromeLauncherFrame& f, ChromeLauncherChrome& c) {
            using TK = LauncherTargetKind;
            for (uint32_t i = 0; i < f.targetCount; ++i) {
                const auto& h = f.targets[i];
                if (h.target.kind != TK::Row || h.target.index != m.FindApp("ladybird")) continue;
                m.BeginPointer(h.target, h.rect.x + 20, h.rect.y + 20);
                c.mouseX = f.rail.x + f.rail.w / 3;
                c.mouseY = f.rail.y + f.rail.h * 9 / 10;
                m.MovePointer({TK::NewCategory, 0}, c.mouseX, c.mouseY, 6, false);
                break;
            } }},
        {"launcher-new-category", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            using TK = LauncherTargetKind;
            m.BeginPointer({TK::Row, uint16_t(m.FindApp("quake2"))}, 20, 20);
            m.MovePointer({TK::NewCategory, 0}, 100, 100, 6, false);
            m.ReleasePointer({TK::NewCategory, 0}, false);
            for (const char* title = "Id Software"; *title; ++title) m.Key(LauncherKey::Char, *title);
            m.Press({TK::EditorColor, uint16_t(LauncherColor::Coral)}, false, 0, 0); }},
        {"launcher-rename-app", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            using TK = LauncherTargetKind;
            m.Press({TK::Organise, 0}, false, 0, 0);
            m.Press({TK::Row, uint16_t(m.FindApp("quake2"))}, false, 0, 0);
            m.Press({TK::RenameApp, 0}, false, 0, 0);
            for (const char* name = "Evening Quake"; *name; ++name) m.Key(LauncherKey::Char, *name); }},
        {"launcher-unsorted", [](LauncherModel& m, const ChromeLauncherFrame&, ChromeLauncherChrome&) {
            using TK = LauncherTargetKind;
            LauncherApp installed[launcherAppCount + 1];
            for (uint32_t i = 0; i < launcherAppCount; ++i) installed[i] = launcherApps[i];
            installed[launcherAppCount] = {"inkscape", "Inkscape", "Vector drawing", "graphics", false};
            m.SetCatalog(installed, launcherAppCount + 1);
            m.MoveMarked(uint32_t(m.FindApp("quake2")), LauncherViewUnsorted);
            m.Press({TK::Rail, uint16_t(m.RailRowFor(LauncherViewUnsorted))}, false, 0, 0);
            m.SetUndoVisible(true); }},
        // The restart confirmation in the footer.
        {"launcher-prompt", [](LauncherModel&, const ChromeLauncherFrame&, ChromeLauncherChrome& c) {
            c.prompt = c.promptRestart = c.confirmFocused = true; }},
    };
    for (const Scene& scene : scenes) {
        model.Init();
        model.SetCatalog(launcherApps, launcherAppCount);
        model.Open();
        ChromeLauncherChrome chrome{icons, launcherAppCount, "brook", "dell-int51-20261119", nullptr, false, false, false, false, -1, -1};
        ChromeLauncherFrameFor(model, chrome, s, screenW, screenH, frame);
        model.SetViewport(frame.viewport);
        ChromeLauncherFrameFor(model, chrome, s, screenW, screenH, frame);
        scene.stage(model, frame, chrome);
        ChromeLauncherFrameFor(model, chrome, s, screenW, screenH, frame);
        model.SetViewport(frame.viewport);
        ChromeLauncherFrameFor(model, chrome, s, screenW, screenH, frame);
        static ChromeShellModel snapshot;
        snapshot = {};
        snapshot.kind = ChromeShellKind::Launcher;
        snapshot.launcherModel = model;
        snapshot.launcherFrame = frame;
        snapshot.mouseX = chrome.mouseX;
        snapshot.mouseY = chrome.mouseY;
        snapshot.prompt = chrome.prompt;
        snapshot.promptRestart = chrome.promptRestart;
        snapshot.confirmFocused = chrome.confirmFocused;
        snapshot.shutdownActive = chrome.shutdownActive;
        std::strcpy(snapshot.launcherBuild, chrome.build);
        for (uint32_t i = 0; i < launcherAppCount; ++i)
            if (icons[i]) SetTestIcon(snapshot.icons[i]);
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(snapshot, s, list);
        const DesktopRect& p = frame.panel;
        Check(prefix + scene.name, list, s, p.x, p.y, p.w, p.h);
    }
}

// --- Shell surfaces -----------------------------------------------------------

DesktopIntervals<> compositorCadence;
const uint64_t now = 100000;

// Use different storage for the received snapshot so all borrowed list assets
// must be rebound locally, as in the shell process.
void CheckShellWire(const ChromeDisplayList& list, const char* label = nullptr) {
    static ShellWireList wire;
    static ChromeCmd replay[ShellCommandLimit];
    if (!PackShellList(list, wire)) std::abort();
    if (label)
        std::printf("%s wire budget: %u/%u commands, %u/%u text bytes, %u/%u asset words\n",
                    label, wire.count, ShellCommandLimit, wire.textBytes, ShellTextLimit, wire.assetWords, ShellAssetLimit);
    for (uint32_t i = 0; i < wire.count; ++i)
        if (!DecodeShellCommand(wire.commands[i], wire.text, wire.textBytes,
                                wire.assets, wire.assetWords, replay[i])) std::abort();
    const ChromeDisplayList decoded(replay, wire.count, wire.text, wire.textBytes, wire.count, wire.textBytes);
    if (ChromeListKey(decoded, 1) != ChromeListKey(list, 1)) std::abort();

    ChromeCmd rejected{};
    const ShellWireCommand invalidText{ChromeOp::Text, 0, 0, 0, 0, 0, wire.textBytes, 0, 0, {}, 0};
    const ShellWireCommand invalidIcon{ChromeOp::Icon, 0, 0, 24, 24, 0, 255, 0, 0, {}, wire.assetWords};
    const ShellWireCommand invalidClip{ChromeOp::Clip, 0, 0, -1, 24, 0, 0, 0, 0, {}, 0};
    if (DecodeShellCommand(invalidText, wire.text, wire.textBytes, wire.assets, wire.assetWords, rejected) ||
        DecodeShellCommand(invalidIcon, wire.text, wire.textBytes, wire.assets, wire.assetWords, rejected) ||
        DecodeShellCommand(invalidClip, wire.text, wire.textBytes, wire.assets, wire.assetWords, rejected)) std::abort();
}

void RenderSnapshot(const ChromeShellModel& model, const ChromeStyle& style, ChromeDisplayList& list) {
    static ChromeShellModel received;
    static ChromeMaterialCache material;
    std::memcpy(&received, &model, sizeof(model));
    const ChromeShellConfig config{style.P(), *style.metrics, style.materialOpacity, style.glass};
    BuildShellModel(received, config.Style(*style.fonts, material), list);
    CheckShellWire(list);
}

void SetTestIcon(ChromeShellIcon& icon) {
    icon.present = true;
    std::memcpy(icon.pixels, testIcon, sizeof(icon.pixels));
}

void LauncherWireBudget(const std::string& name, const ChromeStyle& style, uint32_t screenW, uint32_t screenH) {
    using TK = LauncherTargetKind;
    static ChromeShellModel snapshot;
    static ChromeShellStorage storage;
    static LauncherApp apps[LauncherLimits::Apps];
    snapshot = {};
    snapshot.kind = ChromeShellKind::Launcher;
    snapshot.mouseX = snapshot.mouseY = -1;
    std::strcpy(snapshot.launcherBuild, "Launcher wire-budget fixture");
    auto& model = snapshot.launcherModel;
    model.Init();
    model.SetCatalog(nullptr, 0); // install the fixture apps after the initial layout is filed
    while (model.CategoryCount() < LauncherLimits::Categories) {
        const int category = model.CreateCategory();
        if (category < 0) std::abort();
        char nameBuffer[LauncherLimits::CategoryName];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "Custom category %u", unsigned(category));
        if (!model.RenameCategory(uint32_t(category), nameBuffer)) std::abort();
    }
    for (uint32_t app = 0; app < LauncherLimits::Apps; ++app) {
        auto& entry = apps[app];
        std::snprintf(entry.key, sizeof(entry.key), "org.brook.desktop-fixture.%03u", app);
        std::snprintf(entry.name, sizeof(entry.name), "Application %03u for documents and graphics", app);
        std::strcpy(entry.desc, "Open and edit documents with this workspace writing application");
        std::strcpy(entry.hint, app % 2 ? "office" : "graphics");
        SetTestIcon(snapshot.icons[app]);
    }
    model.SetCatalog(apps, LauncherLimits::Apps);
    for (uint32_t app = 0; app < LauncherLimits::Pins; ++app) model.TogglePin(app);
    model.Press({TK::Organise, 0}, false, 0, 0);
    model.Press({TK::Rail, uint16_t(model.RailRowFor(LauncherViewUnsorted))}, false, 0, 0);
    model.Press({TK::Row, 0}, false, 0, 0);
    model.SetUndoVisible(true);
    if (model.CategoryCount() != LauncherLimits::Categories || model.UnsortedCount() != LauncherLimits::Apps ||
        model.Suggestion(0) < 0 || model.PinCount() != LauncherLimits::Pins) std::abort();
    const ChromeLauncherChrome chrome{nullptr, 0, "brook", snapshot.launcherBuild, nullptr, false, false, false, false, -1, -1};
    auto& frame = snapshot.launcherFrame;
    ChromeLauncherFrameFor(model, chrome, style, screenW, screenH, frame);
    model.SetViewport(frame.viewport);
    ChromeLauncherFrameFor(model, chrome, style, screenW, screenH, frame);
    bool suggestionDrawn = false;
    for (uint32_t i = 0; i < frame.targetCount; ++i) suggestionDrawn |= frame.targets[i].target.kind == TK::MoveSuggested;
    if (!suggestionDrawn) std::abort();
    auto list = ChromeListOn(storage);
    BuildShellModel(snapshot, style, list);
    CheckShellWire(list, name.c_str());
    const auto& rect = frame.panel;
    Check(name, list, style, rect.x, rect.y, rect.w, rect.h);
}

void Shell(const std::string& prefix, uint32_t theme, const DesktopMetrics& m) {
    const bool glass = theme == BROOK_THEME_GLASS;
    const ChromeStyle s = StyleFor(theme, m, glass);
    const uint32_t screenW = 1280 * m.percent / 100, screenH = 800 * m.percent / 100;
    if (prefix == "glass-100-" || prefix == "opaque-dark-150-" || prefix == "opaque-light-200-")
        LauncherWireBudget(prefix + "launcher-organise-budget", s, screenW, screenH);
    static ChromeShellStorage storage;

    // Taskbar: three windows (focused, minimised, plain), a starting app and
    // a failed launch; CPU, MEM, disk and COMP metrics; agent stale.
    const int ids[] = {0, 1, 2, -10, -11};
    const uint32_t metrics = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 4;
    const char* const labels[] = {"Terminal", "Files", "Settings", nullptr, nullptr};
    const ChromeTaskbarLayout layout = ChromeTaskbarLayoutFor(s, screenW, screenH, "14:05 19 Nov 2026", true, metrics, ids, 5, 0,
                                                              "dev  6785ca29  debug", false, labels);
    static ChromeShellModel model;
    model = {};
    model.kind = ChromeShellKind::Taskbar;
    model.taskbar = layout;
    model.entries[0] = {"Terminal", true, false, false, false, 0, LauncherColor::Slate};
    model.entries[1] = {"Files", false, true, false, false, 'F', LauncherColor::Blue};
    model.entries[2] = {"Settings", false, false, false, false, 0, LauncherColor::Slate};
    model.entries[3] = {"", false, false, true, true, 0, LauncherColor::Slate};
    model.entries[4] = {"", false, false, true, false, 'Q', LauncherColor::Coral};
    for (uint32_t i : {0u, 2u, 3u, ChromeShellModel::AppsIcon}) SetTestIcon(model.icons[i]);
    model.networkConnected = true;
    model.volumeControl = 60;
    model.volumeAvailable = true;
    model.agentShown = true;
    model.agentStale = true;
    model.telemetry.stats = {now, 12300000, 400000, 0, 0, 42, 73};
    model.telemetry.hasCompositor = true;
    model.telemetry.compositor = compositorCadence;
    model.uptimeMs = now + 300; // the cadence samples are recent, ring step 3
    for (int hover = 0; hover < 2; ++hover) {
        const DesktopRect& target = layout.buttons[1];
        model.mouseX = hover ? target.x + 2 : -1;
        model.mouseY = hover ? target.y + 2 : -1;
        model.launcherOpen = hover;
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(model, s, list);
        Check(prefix + (hover ? "taskbar-hover" : "taskbar"), list, s, 0, layout.y, screenW, screenH - layout.y);
    }
    // The Dell's logical width (3200 at 200%): every diagnostic on, and the
    // default set (CPU, MEM, temperature), with a payload build label.
    for (const bool all : {true, false}) {
        const uint32_t wideW = 1600 * m.percent / 100;
        const uint32_t set = all ? 127u : (1u << 0 | 1u << 1 | 1u << 6);
        const ChromeTaskbarLayout wide = ChromeTaskbarLayoutFor(s, wideW, screenH, "14:05 19 Nov 2026", false, set, ids, 3, 0,
                                                                "dell-int51-20261119", false, labels);
        static ChromeShellModel wm;
        wm = model;
        wm.taskbar = wide;
        wm.agentShown = false;
        wm.mouseX = wm.mouseY = -1;
        wm.launcherOpen = false;
        wm.telemetry.hasApp = true;
        wm.telemetry.app = compositorCadence;
        wm.telemetry.stats.downBytesPerSecond = 2400000;
        wm.telemetry.stats.upBytesPerSecond = 150000;
        wm.telemetry.thermal = true; wm.telemetry.tjmax = 100; wm.telemetry.latest.pkgC = 57;
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(wm, s, list);
        Check(prefix + (all ? "taskbar-diagnostics" : "taskbar-default"), list, s, 0, wide.y, wideW, screenH - wide.y);
    }
    // The compact HUD and the temperature target, without the diagnostics.
    const ChromeTaskbarLayout hudLayout = ChromeTaskbarLayoutFor(s, screenW, screenH, "09:41 15 Nov 2026", false,
                                                                  1u << 6, ids, 3, 2, "Brook 3a4b909e", true, labels);
    {
        model.taskbar = hudLayout;
        model.agentShown = false;
        model.mouseX = model.mouseY = -1;
        model.launcherOpen = false;
        model.telemetry.thermal = true;
        model.telemetry.tjmax = 100;
        model.telemetry.latest.pkgC = 57;
        model.compactTelemetry = model.telemetry;
        model.hudTick = now + 300;
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(model, s, list);
        Check(prefix + "taskbar-hud", list, s, 0, hudLayout.y, screenW, screenH - hudLayout.y);
    }

    {
        model.tray = {};
        model.tray.itemCount = 2;
        model.tray.items[0].id = 1;
        model.tray.items[0].hasIcon = true;
        std::memcpy(model.tray.items[0].icon, testIcon, sizeof(testIcon));
        std::strcpy(model.tray.items[0].title, "Steam");
        model.tray.items[1].id = 2;
        model.tray.items[1].attention = true;
        std::strcpy(model.tray.items[1].title, "Updater");
        model.taskbar = ChromeTaskbarLayoutFor(s, screenW, screenH, "14:05 19 Nov 2026", false,
                                              metrics, ids, 3, 0, nullptr, false, labels, 2);
        const auto anchor = model.taskbar.trayApps[0];
        model.mouseX = anchor.x + 5; model.mouseY = anchor.y + 5;
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(model, s, list);
        Check(prefix + "app-tray", list, s, 0, model.taskbar.y, screenW, screenH - model.taskbar.y);

        model.kind = ChromeShellKind::AppTrayMenu;
        model.tray.menuOwner = 1;
        model.tray.menuCount = 4;
        model.tray.menu[0] = {1, "Show Library", true, false, false};
        model.tray.menu[1] = {2, "", false, true, false};
        model.tray.menu[2] = {3, "Recent games", true, false, true};
        model.tray.menu[3] = {4, "Exit", false, false, false};
        model.rect = ChromeTrayMenuRect(s, anchor, model.tray.menuCount, screenW);
        model.mouseX = model.rect.x + int(s.M(14));
        model.mouseY = model.rect.y + int(s.M(18));
        auto menuList = ChromeListOn(storage);
        RenderSnapshot(model, s, menuList);
        Check(prefix + "app-tray-menu", menuList, s, model.rect.x, model.rect.y, model.rect.w, model.rect.h);

        model.kind = ChromeShellKind::Notification;
        model.tray.notice = {42, 2, "Steam", "Download complete", "Quake II is ready to play", {"Open library", "Dismiss"}};
        model.rect = ChromeNoticeRect(s, screenW, model.taskbar.y, model.tray.notice.actionCount);
        model.mouseX = model.rect.x + int(s.M(20));
        model.mouseY = model.rect.y + model.rect.h - int(s.M(29));
        auto noticeList = ChromeListOn(storage);
        RenderSnapshot(model, s, noticeList);
        Check(prefix + "app-notification", noticeList, s, model.rect.x, model.rect.y, model.rect.w, model.rect.h);
    }

    Launcher(prefix, s, screenW, screenH);

    // Volume popup over the tray icon, mute button hovered.
    {
        model = {};
        model.kind = ChromeShellKind::VolumePopup;
        auto& vm = model.volume;
        vm.layout = TaskbarVolumePopup(m, layout.volume, layout.y, screenW, ChromeVolumeValueWidth(s));
        vm.control = 60;
        vm.available = true;
        vm.mouseX = vm.layout.mute.x + 1;
        vm.mouseY = vm.layout.mute.y + 1;
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(model, s, list);
        const DesktopRect& p = vm.layout.panel;
        Check(prefix + "volume-popup", list, s, p.x, p.y, p.w, p.h);
    }

    // Tray tooltip.
    {
        const char* text = "Volume: 60%";
        const DesktopRect box = TaskbarTooltipBox(layout.volume, ChromeTooltipWidth(s, text, nullptr),
                                                  ChromeTooltipHeight(s, false), layout.y,
                                                  static_cast<int>(s.M(8)), screenW, static_cast<int>(s.M(4)));
        ChromeDisplayList list = ChromeListOn(storage);
        model = {};
        model.kind = ChromeShellKind::Tooltip;
        model.rect = box;
        std::strcpy(model.text, text);
        RenderSnapshot(model, s, list);
        Check(prefix + "tooltip", list, s, box.x, box.y, box.w, box.h);
    }

    // Boot toast: the total, the four phases and three meta lines.
    {
        const int margin = s.M(16), w = s.M(360), h = ChromeBootSummaryHeight(s, true, true, 3);
        model = {};
        model.kind = ChromeShellKind::BootSummary;
        model.rect = {static_cast<int>(screenW) - w - margin, margin, w, h};
        auto& boot = model.boot;
        std::strcpy(boot.total, "8.12 s");
        std::strcpy(boot.totalNote, "from power-on");
        std::strcpy(boot.meta[0], "8 cores " BROOK_CHROME_DOT " 7712 MiB available");
        std::strcpy(boot.meta[1], "Software composition " BROOK_CHROME_DOT " Opaque");
        std::strcpy(boot.meta[2], "Brook 3a4b909e+29117654");
        static const char* const names[] = {"Firmware", "Menu", "Loader", "Kernel"};
        static const char* const values[] = {"3.41 s", "0.52 s", "0.61 s", "3.58 s"};
        const uint64_t ms[] = {3410, 520, 610, 3580};
        for (unsigned i = 0; i < 4; ++i) {
            boot.phaseMs[i] = ms[i];
            std::strcpy(boot.phaseNames[i], names[i]);
            std::strcpy(boot.phaseValues[i], values[i]);
        }
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(model, s, list);
        Check(prefix + "boot-summary", list, s, model.rect.x, model.rect.y, w, h);
    }

    // Tray flyouts with live content, and the HUD panel with a thermal graph.
    model = {};
    model.kind = ChromeShellKind::TrayFlyout;
    auto& telemetry = model.telemetry;
    telemetry.hasApp = telemetry.hasCompositor = true;
    telemetry.app = telemetry.compositor = compositorCadence;
    telemetry.tjmax = 100;
    telemetry.thermal = telemetry.rapl = true;
    telemetry.pointCount = 60;
    for (unsigned i = 0; i < telemetry.pointCount; ++i) {
        auto& point = telemetry.points[i];
        point.pkgC = static_cast<int16_t>(52 + (i * 7) % 6);
        point.pkgMw = 6200;
    }
    telemetry.latest = telemetry.points[telemetry.pointCount - 1];
    model.networkConnected = true;
    std::strcpy(model.text, "192.168.1.35");
    for (const bool network : {true, false}) {
        model.network = network;
        const DesktopRect flyout{0, 0, int(s.M(304)), ChromeTrayFlyoutHeight(s, network, true)};
        model.rect = flyout;
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(model, s, list);
        Check(prefix + (network ? "network-flyout" : "temperature-flyout"), list, s, flyout.x, flyout.y, flyout.w, flyout.h);
    }
    model.kind = ChromeShellKind::PerformanceHud;
    const DesktopRect hud{0, 0, int(s.M(348)), ChromePerformanceHudHeight(s, true)};
    model.rect = hud;
    model.hudTick = now + 300;
    ChromeDisplayList list = ChromeListOn(storage);
    RenderSnapshot(model, s, list);
    Check(prefix + "hud", list, s, hud.x, hud.y, hud.w, hud.h);
}

// Side dock: the strip open on a page, the same strip pinned with an icon
// hovered, the return button hovered, and the closed edge tab.
void Dock(const std::string& prefix, uint32_t theme, const DesktopMetrics& m) {
    const bool glass = theme == BROOK_THEME_GLASS;
    const ChromeStyle s = StyleFor(theme, m, glass);
    const uint32_t screenW = 1600 * m.percent / 100, desktopH = 852 * m.percent / 100;
    static ChromeShellStorage storage;
    static ChromeShellModel model;
    // hover: -1, entry, 100 return, 200 the side button; resizing: the grip is dragged.
    struct Scene { const char* name; int selected; bool pinned; int hover; bool right = false; int width = 0;
                   bool resizing = false; };
    const Scene scenes[] = {
        {"dock-open", 0, false, -1},
        {"dock-selected", 2, false, 100},
        {"dock-pinned", 1, true, 3},
        {"dock-right", 1, false, 200, true, 520},
        {"dock-resizing", 0, true, -1, false, 360, true},
    };
    for (const Scene& scene : scenes) {
        model = {};
        model.kind = ChromeShellKind::Dock;
        model.dock = ChromeDockLayoutFor(m, screenW, desktopH, 4, scene.selected, true, {scene.width, scene.right});
        model.dockPinned = scene.pinned;
        model.dockResizing = scene.resizing;
        for (int i = 0; i < 4; ++i) if (i != 3) SetTestIcon(model.icons[i]); // entry 3: missing icon
        std::strcpy(model.text, "Terminal - brook@clearwater: ~/src/brook");
        model.mouseX = model.mouseY = -1;
        if (scene.hover == 100) { model.mouseX = model.dock.back.x + 2; model.mouseY = model.dock.back.y + 2; }
        else if (scene.hover == 200) { model.mouseX = model.dock.side.x + 2; model.mouseY = model.dock.side.y + 2; }
        else if (scene.hover >= 0) {
            model.mouseX = model.dock.cells[scene.hover].x + 2;
            model.mouseY = model.dock.cells[scene.hover].y + 2;
        }
        ChromeDisplayList list = ChromeListOn(storage);
        RenderSnapshot(model, s, list);
        const DesktopRect& r = model.dock.strip;
        Check(prefix + scene.name, list, s, r.x, r.y, r.w, r.h);
    }
    model = {};
    model.kind = ChromeShellKind::Dock;
    model.dock = ChromeDockLayoutFor(m, screenW, desktopH, 2, -1, false);
    model.mouseX = model.mouseY = -1;
    ChromeDisplayList list = ChromeListOn(storage);
    RenderSnapshot(model, s, list);
    const DesktopRect& t = model.dock.tab;
    Check(prefix + "dock-tab", list, s, 0, t.y - m.pixels(8), m.pixels(24), t.h + m.pixels(16));
}

void WebAppTitles(const std::string& prefix, uint32_t theme, const DesktopMetrics& m) {
    const ChromeStyle s = StyleFor(theme, m, theme == BROOK_THEME_GLASS);
    char title[64];
    FormatWebAppTitle(title, "Brook Mail", "Inbox");
    if (s.TextWidth("\xe2\x80\x93") <= 0 || s.TextWidth("A\xc3\x81" "B") != s.TextWidth("AB")) {
        std::fprintf(stderr, "%s dash glyph encoding mismatch\n", prefix.c_str());
        ++failures;
    }

    ChromeFrameModel frame{};
    frame.clientW = m.pixels(360);
    frame.layout = ChromeFrameLayout({{0, 0, frame.clientW, m.pixels(80), false}, false, {}}, m);
    frame.focused = true;
    frame.title = title;
    frame.icon = testIcon;
    frame.mouseX = frame.mouseY = -1;
    ChromeFrameStorage frameStorage;
    ChromeDisplayList list = ChromeListOn(frameStorage);
    ChromeBuildFrame(frame, s, list);
    const DesktopRect outer = frame.layout.outer;
    Check(prefix + "web-app-frame", list, s, outer.x, outer.y, outer.w, outer.h);

    constexpr uint32_t sw = 800, sh = 600;
    const int ids[] = {0};
    const char* labels[] = {title};
    const auto taskbar = ChromeTaskbarLayoutFor(s, sw, sh, "14:05 19 Nov 2026", false, 0, ids, 1, 0,
                                               "", false, labels);
    static ChromeShellModel model;
    static ChromeShellStorage storage;
    model = {};
    model.kind = ChromeShellKind::Taskbar;
    model.taskbar = taskbar;
    model.entries[0].active = true;
    std::strcpy(model.entries[0].label, title);
    SetTestIcon(model.icons[0]);
    ChromeDisplayList taskbarList = ChromeListOn(storage);
    RenderSnapshot(model, s, taskbarList);
    Check(prefix + "web-app-taskbar", taskbarList, s, 0, taskbar.y, sw, sh - taskbar.y);

    const auto box = TaskbarTooltipBox(taskbar.buttons[0], ChromeTooltipWidth(s, title, nullptr),
                                      ChromeTooltipHeight(s, false), taskbar.y, 8, sw, 4);
    model = {};
    model.kind = ChromeShellKind::Tooltip;
    model.rect = box;
    std::strcpy(model.text, title);
    ChromeDisplayList tooltipList = ChromeListOn(storage);
    RenderSnapshot(model, s, tooltipList);
    Check(prefix + "web-app-tooltip", tooltipList, s, box.x, box.y, box.w, box.h);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--update")) update = true;
    MakeIcon();
    for (unsigned i = 0; i < 40; ++i) compositorCadence.Record(now - 2000 + i * 16 + (i % 3) * 3);
    {
        const std::vector<uint8_t> text = ReadFile(goldenDir + "/MANIFEST");
        std::string all(text.begin(), text.end());
        size_t pos = 0;
        while (pos < all.size()) {
            const size_t end = all.find('\n', pos);
            const std::string line = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            const size_t space = line.find(' ');
            if (space != std::string::npos && line[0] != '#') oldManifest[line.substr(0, space)] = line.substr(space + 1);
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }
    // Cover every material and mode at each scale in Blue. Compare the
    // accent mapping in a few other presets and custom magenta at 100%.
    static const GoldenTheme themes[] = {
        {"glass-light", BROOK_THEME_GLASS, BROOK_MODE_LIGHT, BROOK_HUE_BLUE},
        {"glass", BROOK_THEME_GLASS, BROOK_MODE_MIXED, BROOK_HUE_BLUE},
        {"glass-dark", BROOK_THEME_GLASS, BROOK_MODE_DARK, BROOK_HUE_BLUE},
        {"opaque-light", BROOK_THEME_OPAQUE, BROOK_MODE_LIGHT, BROOK_HUE_BLUE},
        {"opaque-mixed", BROOK_THEME_OPAQUE, BROOK_MODE_MIXED, BROOK_HUE_BLUE},
        {"opaque-dark", BROOK_THEME_OPAQUE, BROOK_MODE_DARK, BROOK_HUE_BLUE},
        {"amber-glass", BROOK_THEME_GLASS, BROOK_MODE_MIXED, BROOK_HUE_AMBER},
        {"teal-opaque-light", BROOK_THEME_OPAQUE, BROOK_MODE_LIGHT, BROOK_HUE_TEAL},
        {"magenta-glass-light", BROOK_THEME_GLASS, BROOK_MODE_LIGHT, BROOK_HUE_CUSTOM, 0xff00ff},
        {"magenta-glass-mixed", BROOK_THEME_GLASS, BROOK_MODE_MIXED, BROOK_HUE_CUSTOM, 0xff00ff},
        {"magenta-glass-dark", BROOK_THEME_GLASS, BROOK_MODE_DARK, BROOK_HUE_CUSTOM, 0xff00ff},
        {"magenta-opaque-light", BROOK_THEME_OPAQUE, BROOK_MODE_LIGHT, BROOK_HUE_CUSTOM, 0xff00ff},
        {"magenta-opaque-mixed", BROOK_THEME_OPAQUE, BROOK_MODE_MIXED, BROOK_HUE_CUSTOM, 0xff00ff},
        {"magenta-opaque-dark", BROOK_THEME_OPAQUE, BROOK_MODE_DARK, BROOK_HUE_CUSTOM, 0xff00ff},
    };
    for (const GoldenTheme& theme : themes) {
        SetPalette(theme);
        for (const DesktopMetrics* m : {&DesktopMetric100, &DesktopMetric150, &DesktopMetric200}) {
            if (theme.hue != BROOK_HUE_BLUE && m->percent != 100) continue;
            const std::string prefix = std::string(theme.name) + "-" + std::to_string(m->percent) + "-";
            Frames(prefix, theme.material, *m);
            Shell(prefix, theme.material, *m);
            if (m->percent == 100 && theme.hue == BROOK_HUE_BLUE && theme.mode == BROOK_MODE_MIXED)
                WebAppTitles(prefix, theme.material, *m);
            // The side dock in Glass (mixed, at 100% and 200%) and Opaque Dark.
            if (theme.hue == BROOK_HUE_BLUE && m->percent != 150 &&
                ((theme.material == BROOK_THEME_GLASS && theme.mode == BROOK_MODE_MIXED) ||
                 (theme.material == BROOK_THEME_OPAQUE && theme.mode == BROOK_MODE_DARK && m->percent == 100)))
                Dock(prefix, theme.material, *m);
        }
    }
    if (update) {
        std::string text = "# name FNV-1a of the ARGB buffer (alpha included); see test_chrome_goldens.\n";
        for (const auto& e : manifest) text += e.first + " " + e.second + "\n";
        WriteFile(goldenDir + "/MANIFEST", std::vector<uint8_t>(text.begin(), text.end()));
        std::printf("CHROME_GOLDENS_UPDATED %d images\n", written);
        return 0;
    }
    if (manifest.size() != oldManifest.size()) {
        std::fprintf(stderr, "MANIFEST lists %zu images, the test renders %zu\n", oldManifest.size(), manifest.size());
        ++failures;
    }
    if (failures) { std::fprintf(stderr, "CHROME_GOLDENS: %d failures of %d\n", failures, compared); return 1; }
    std::printf("CHROME_GOLDENS_OK %d images\n", compared);
    return 0;
}

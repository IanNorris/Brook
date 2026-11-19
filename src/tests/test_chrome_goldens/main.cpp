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
#include "chrome_shell.h"
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

// A Clearwater theme as the painters see it: (theme, hue), Glass in Mixed
// mode (dark chrome). Glass goldens rasterise explicit alpha for capture;
// opaque goldens exercise the solid CPU fallback.
struct GoldenTheme { const char* name; unsigned theme, hue; };
BrookThemePalette palettes[8];
const BrookThemePalette* PaletteFor(unsigned index, const GoldenTheme& t) {
    BrookHueTokens tokens;
    brook_hue_derive(t.hue, &tokens);
    brook_theme_build_palette(&tokens, t.theme, brook_theme_chrome_dark(t.theme, BROOK_MODE_MIXED), &palettes[index]);
    return &palettes[index];
}
const BrookThemePalette* currentPalette;

ChromeStyle StyleFor(uint32_t, const DesktopMetrics& m, bool glass) {
    static const ChromeFontSet* fonts[] = {&g_chromeFonts100, &g_chromeFonts150, &g_chromeFonts200};
    const ChromeFontSet* font = fonts[m.percent == 100 ? 0 : m.percent == 150 ? 1 : 2];
    const BrookThemePalette* p = currentPalette;
    ChromeStyle s{p, &m, font, glass, glass ? 200u : 255u, nullptr};
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
    struct Case { const char* name; bool focused, maximised, glass; const uint32_t* icon; int hover; };
    // hover: -1 none; 0..2 a caption button by its outer top-right corner of
    // its hit rect (the bleed area, not the visual cell).
    const Case cases[] = {
        {"frame-active", true, false, false, testIcon, -1},
        {"frame-inactive", false, false, false, nullptr, -1},
        {"frame-hover-close", true, false, false, testIcon, 0},
        {"frame-hover-max", true, false, false, testIcon, 1},
        {"frame-hover-min", false, false, false, testIcon, 2},
        {"frame-maximised", true, true, false, testIcon, -1},
        {"frame-active-glass", true, false, true, testIcon, -1},
    };
    for (const Case& c : cases) {
        const ChromeStyle s = StyleFor(theme, m, c.glass);
        ChromeFrameModel model{};
        model.layout = ChromeFrameLayout({{0, 0, clientW, clientH, false}, c.maximised, {}}, m);
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

// --- Shell surfaces -----------------------------------------------------------

DesktopIntervals<> compositorCadence;
const uint64_t now = 100000;

void Shell(const std::string& prefix, uint32_t theme, const DesktopMetrics& m) {
    const bool glass = theme == 0 || theme == 3;
    const ChromeStyle s = StyleFor(theme, m, glass);
    const uint32_t screenW = 1280 * m.percent / 100, screenH = 800 * m.percent / 100;
    static ChromeShellStorage storage;

    // Taskbar: three windows (focused, minimised, plain), a starting app and
    // a failed launch; CPU, MEM, disk and COMP metrics; agent stale.
    const int ids[] = {0, 1, 2, -10, -11};
    const uint32_t metrics = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 4;
    const char* const labels[] = {"Terminal", "Files", "Settings", nullptr, nullptr};
    const ChromeTaskbarLayout layout = ChromeTaskbarLayoutFor(s, screenW, screenH, "14:05 19 Nov 2026", true, metrics, ids, 5, 0,
                                                              "dev  6785ca29  debug", false, labels);
    const ChromeTaskbarButton buttons[] = {
        {testIcon, "Terminal", true, false, false, false},
        {nullptr, "Files", false, true, false, false},
        {testIcon, "Settings", false, false, false, false},
        {testIcon, "Starting: Quake II", false, false, true, true},
        {nullptr, "Launch failed: Chromium", false, false, true, false},
    };
    ChromeTaskbarModel tb{};
    tb.layout = &layout;
    tb.buttons = buttons;
    tb.appsIcon = testIcon;
    tb.terminalIcon = nullptr;
    tb.networkConnected = true;
    tb.volumeControl = 60;
    tb.volumeAvailable = true;
    tb.agentShown = true;
    tb.agentStale = true;
    tb.telemetry.stats = {now, 12300000, 400000, 0, 0, 42, 73};
    tb.telemetry.compositor = &compositorCadence;
    tb.uptimeMs = now + 300; // the cadence samples are recent; ring step 3
    for (int hover = 0; hover < 2; ++hover) {
        const DesktopRect& target = layout.buttons[1];
        tb.mouseX = hover ? target.x + 2 : -1;
        tb.mouseY = hover ? target.y + 2 : -1;
        tb.launcherOpen = hover;
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildTaskbar(tb, s, list);
        Check(prefix + (hover ? "taskbar-hover" : "taskbar"), list, s, 0, layout.y, screenW, screenH - layout.y);
    }
    // The compact HUD and the temperature target, without the diagnostics.
    {
        const ChromeTaskbarLayout hudLayout = ChromeTaskbarLayoutFor(s, screenW, screenH, "09:41 15 Nov 2026", false, 1u << 6,
                                                                     ids, 3, 2, "Brook 3a4b909e", true, labels);
        ChromeTaskbarModel hm = tb;
        hm.layout = &hudLayout;
        hm.agentShown = false;
        hm.mouseX = hm.mouseY = -1;
        hm.launcherOpen = false;
        hm.telemetry.thermal = true; hm.telemetry.tjmax = 100; hm.telemetry.latest.pkgC = 57;
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildTaskbar(hm, s, list);
        ChromeBuildCompactHud({hudLayout.hud, hm.telemetry, now + 300}, s, list);
        Check(prefix + "taskbar-hud", list, s, 0, hudLayout.y, screenW, screenH - hudLayout.y);
    }

    // Launcher: ten apps, the third hovered; then the restart prompt.
    const ChromeLauncherItem items[] = {
        {"Terminal", testIcon}, {"Files", nullptr}, {"Settings", testIcon}, {"Chromium", testIcon},
        {"Quake II", nullptr}, {"Stardew Valley", testIcon}, {"Text Editor", testIcon}, {"Gallery", nullptr},
        {"Kernel Console", testIcon}, {"Steam", testIcon},
    };
    for (int prompt = 0; prompt < 2; ++prompt) {
        ChromeLauncherModel lm{};
        lm.layout = ChromeLauncherLayoutFor(s, screenW, screenH, 10);
        lm.header = prompt ? "Restart? Open apps will be asked to close" : "Applications";
        lm.items = items;
        lm.count = 10;
        lm.prompt = prompt;
        lm.promptRestart = true;
        lm.confirmFocused = true;
        const DesktopRect hovered = ChromeLauncherItemRect(s, lm.layout, 2);
        lm.mouseX = hovered.x + 3;
        lm.mouseY = hovered.y + 3;
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildLauncher(lm, s, list);
        Check(prefix + (prompt ? "launcher-prompt" : "launcher"), list, s, lm.layout.x, lm.layout.y,
              lm.layout.w, lm.layout.h);
    }

    // Volume popup over the tray icon, mute button hovered.
    {
        ChromeVolumePopupModel vm{};
        vm.layout = TaskbarVolumePopup(m, layout.volume, layout.y, screenW, ChromeVolumeValueWidth(s));
        vm.control = 60;
        vm.available = true;
        vm.mouseX = vm.layout.mute.x + 1;
        vm.mouseY = vm.layout.mute.y + 1;
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildVolumePopup(vm, s, list);
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
        ChromeBuildTooltip({box, text}, s, list);
        Check(prefix + "tooltip", list, s, box.x, box.y, box.w, box.h);
    }

    // Boot toast: the total, the four phases and three meta lines.
    {
        const int margin = s.M(16), w = s.M(360), h = ChromeBootSummaryHeight(s, true, true, 3);
        ChromeBootSummaryModel bm{{static_cast<int>(screenW) - w - margin, margin, w, h}, "8.12 s", "from power-on",
                                  {"8 cores " BROOK_CHROME_DOT " 7712 MiB available",
                                   "Software composition " BROOK_CHROME_DOT " Opaque", "Brook 3a4b909e+29117654"}};
        static const char* const names[] = {"Firmware", "Menu", "Loader", "Kernel"};
        static const char* const values[] = {"3.41 s", "0.52 s", "0.61 s", "3.58 s"};
        const uint64_t ms[] = {3410, 520, 610, 3580};
        for (unsigned i = 0; i < 4; ++i) { bm.phaseMs[i] = ms[i]; bm.phaseNames[i] = names[i]; bm.phaseValues[i] = values[i]; }
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildBootSummary(bm, s, list);
        Check(prefix + "boot-summary", list, s, bm.rect.x, bm.rect.y, w, h);
    }

    // Tray flyouts with live content, and the HUD panel with a thermal graph.
    ChromeTelemetryModel telemetry = tb.telemetry;
    static ThermalSample points[60];
    for (unsigned i = 0; i < 60; ++i) { points[i].pkgC = static_cast<int16_t>(52 + (i * 7) % 6); points[i].pkgMw = 6200; }
    telemetry.tjmax = 100; telemetry.thermal = true; telemetry.rapl = true;
    telemetry.points = points; telemetry.pointCount = 60; telemetry.latest = points[59];
    telemetry.app = &compositorCadence;
    for (const bool network : {true, false}) {
        const DesktopRect flyout{0, 0, int(s.M(304)), ChromeTrayFlyoutHeight(s, network, true)};
        ChromeDisplayList list = ChromeListOn(storage);
        ChromeBuildTrayFlyout({flyout, network, true, "192.168.1.35", telemetry}, s, list);
        Check(prefix + (network ? "network-flyout" : "temperature-flyout"), list, s,
              flyout.x, flyout.y, flyout.w, flyout.h);
    }
    const DesktopRect hud{0, 0, int(s.M(348)), ChromePerformanceHudHeight(s, true)};
    ChromeDisplayList list = ChromeListOn(storage);
    ChromeBuildPerformanceHud({hud, telemetry, now + 300}, s, list);
    Check(prefix + "hud", list, s, hud.x, hud.y, hud.w, hud.h);
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
    // The three themes in Blue at every scale (stage 2.2), and two more hues
    // at 100% (stage 2.3: a painter that hard-codes a blue shows up here).
    static const GoldenTheme themes[] = {
        {"glass", BROOK_THEME_GLASS, BROOK_HUE_BLUE},
        {"opaque-light", BROOK_THEME_OPAQUE_LIGHT, BROOK_HUE_BLUE},
        {"opaque-dark", BROOK_THEME_OPAQUE_DARK, BROOK_HUE_BLUE},
        {"amber-glass", BROOK_THEME_GLASS, BROOK_HUE_AMBER},
        {"teal-opaque-light", BROOK_THEME_OPAQUE_LIGHT, BROOK_HUE_TEAL},
    };
    for (uint32_t theme = 0; theme < sizeof(themes) / sizeof(themes[0]); ++theme) {
        currentPalette = PaletteFor(theme, themes[theme]);
        for (const DesktopMetrics* m : {&DesktopMetric100, &DesktopMetric150, &DesktopMetric200}) {
            if (theme >= 3 && m->percent != 100) continue;
            const std::string prefix = std::string(themes[theme].name) + "-" + std::to_string(m->percent) + "-";
            Frames(prefix, theme, *m);
            Shell(prefix, theme, *m);
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

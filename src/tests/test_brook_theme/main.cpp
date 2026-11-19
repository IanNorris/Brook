// Clearwater token vectors and contrast from DESIGN.md. Check the colour
// maths, independent material/mode, settings migration and custom accents.
#include "../../shared/brook_theme.h"
#include "../../shared/brook_appearance.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"BROOK_THEME line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static int failures = 0;

struct Vector {
    const char* key;
    struct { uint32_t glass, title, flat, ink; unsigned ink2, ink3; uint32_t shadow; } dark;
    struct { uint32_t glass, title, flat, ink; unsigned ink2, ink3; uint32_t line; unsigned lineA; uint32_t divider; unsigned dividerA;
             uint32_t hover; unsigned hoverA; uint32_t pressed; unsigned pressedA; uint32_t shadow; } light;
    struct { uint32_t bg, raised, sunken, line, ink, dim; } contentDark, contentLight;
};
static const Vector vectors[BROOK_HUE_COUNT] = {
    {"blue", {0x102442,0x28508a,0x162844,0xeef4fc,74,52,0x030a16}, {0xe2edfb,0xbad5f8,0xe2ebf7,0x14263d,78,66,0x142c50,24,0x142c50,13,0x143264,8,0x143264,14,0x142238}, {0x111c2b,0x18263a,0x0c1522,0x26374f,0xe6edf7,0x93a4bb}, {0xf5f8fc,0xffffff,0xeaf0f8,0xd7e0ed,0x1c2b3f,0x5c6d84}},
    {"navy", {0x00114b,0x0031a8,0x001453,0xeff3fc,74,52,0x050916}, {0xe2ecff,0xbcd3ff,0xe0ebff,0x1a243e,78,66,0x1e2a50,24,0x1e2a50,13,0x222f64,8,0x222f64,14,0x192138}, {0x0c1328,0x131d38,0x070d1f,0x212d4e,0xe8ecf7,0x97a2bc}, {0xf6f8fc,0xffffff,0xebeff8,0xd9dfed,0x212a3f,0x616b84}},
    {"teal", {0x002b27,0x005e56,0x00302b,0xecf6f4,74,52,0x000d0b}, {0xd6f3ef,0x96e4da,0xd8f0ec,0x062c28,78,66,0x003532,24,0x003532,13,0x003d3b,8,0x003d3b,14,0x052826}, {0x0a201e,0x0d2b29,0x061817,0x1a3d3a,0xe3efee,0x8caaa6}, {0xf4f9f8,0xffffff,0xe8f2f0,0xd3e3e1,0x13302d,0x54736f}},
    {"pine", {0x002d19,0x00613c,0x00311c,0xedf6f1,74,52,0x010d07}, {0xdaf3e4,0xa1e5be,0xdbf0e3,0x0a2c1e,78,66,0x003625,24,0x003625,13,0x003f2d,8,0x003f2d,14,0x07291d}, {0x0c2017,0x102c20,0x071911,0x1d3e30,0xe4f0ea,0x8eaa9c}, {0xf4f9f6,0xffffff,0xe9f2ed,0xd4e4dc,0x153124,0x577365}},
    {"phosphor", {0x002e0c,0x006222,0x00320e,0xeef6ef,74,52,0x020d04}, {0xddf3df,0xaae3b1,0xddf0df,0x112c18,78,66,0x03371b,24,0x03371b,13,0x004021,8,0x004021,14,0x0d2818}, {0x0f2014,0x142b1b,0x09190e,0x223d2a,0xe5f0e7,0x91aa96}, {0xf5f9f6,0xffffff,0xeaf2eb,0xd6e3d9,0x1a301f,0x5a7360}},
    {"lime", {0x1c2a00,0x425b00,0x1f2e00,0xf1f5ec,74,52,0x070c01}, {0xe6f1d7,0xc4de9c,0xe5eed9,0x1f2a0d,78,66,0x213305,24,0x213305,13,0x243d00,8,0x243d00,14,0x1b260d}, {0x171e0d,0x202912,0x111708,0x2f3b1f,0xe9efe4,0x9ca78d}, {0xf7f9f4,0xffffff,0xedf1e8,0xdbe2d4,0x252e16,0x657056}},
    {"amber", {0x453100,0x8e6800,0x453100,0xf7f3ea,74,56,0x0f0900}, {0xffe9c0,0xffca59,0xffe7ba,0x302304,78,66,0x392a00,24,0x392a00,13,0x423200,8,0x423200,14,0x2b2004}, {0x241a02,0x312301,0x1c1301,0x44340a,0xf1ece1,0xada187}, {0xfaf7f3,0xffffff,0xf3efe6,0xe5dfd0,0x342910,0x776a50}},
    {"orange", {0x512000,0x924000,0x572300,0xfbf1ec,74,56,0x130601}, {0xffe6d9,0xffc3a4,0xffe4d6,0x381e0d,78,66,0x472100,24,0x472100,13,0x512700,8,0x512700,14,0x331c0a}, {0x2a1608,0x381e0b,0x211005,0x4d2d17,0xf5eae3,0xb79c8c}, {0xfbf7f4,0xffffff,0xf7ede8,0xebdcd3,0x3b2416,0x806555}},
    {"red", {0x590001,0x990004,0x600002,0xfcf0ef,74,56,0x140504}, {0xffe5e1,0xffc1b8,0xffe3de,0x3a1b18,78,66,0x4a1c15,24,0x4a1c15,13,0x5b1c11,8,0x5b1c11,14,0x351914}, {0x2c1310,0x3b1b16,0x230e0a,0x512924,0xf7e9e6,0xba9a95}, {0xfcf6f5,0xffffff,0xf8eceb,0xecdbd7,0x3d221e,0x82635e}},
    {"pink", {0x450528,0x900e58,0x470e2c,0xfbf0f4,74,52,0x13050b}, {0xffe5ef,0xffbcd7,0xfee5ee,0x371b28,78,66,0x471b2f,24,0x471b2f,13,0x581a35,8,0x581a35,14,0x331823}, {0x28151d,0x361d27,0x1f0f16,0x4a2c38,0xf5e8ed,0xb799a5}, {0xfbf6f8,0xffffff,0xf7ecf0,0xebdae0,0x3a212c,0x7f626e}},
    {"violet", {0x28164f,0x5833a3,0x2b1c50,0xf4f2fb,74,52,0x0b0715}, {0xece8ff,0xd4caff,0xeae7fd,0x28203a,78,66,0x32234b,24,0x32234b,13,0x3d255c,8,0x3d255c,14,0x261c35}, {0x1d1829,0x282137,0x171220,0x39314c,0xedeaf6,0xa59eb8}, {0xf8f7fc,0xffffff,0xf0eef7,0xe1ddeb,0x2d263d,0x6e6781}},
    {"slate", {0x172538,0x365276,0x1c293a,0xf2f3f6,74,52,0x070a0e}, {0xe6ecf6,0xc3d4eb,0xe5ebf3,0x21262d,78,66,0x262d38,24,0x262d38,13,0x2c3543,8,0x2c3543,14,0x1e2229}, {0x181c20,0x22262c,0x121519,0x32373e,0xeaecf0,0x9ea3aa}, {0xf7f8f9,0xffffff,0xeeeff2,0xdddfe3,0x262b31,0x676c73}},
    {"wallpaper", {0x3a1b00,0x794000,0x3e1f02,0xf8f2ee,74,52,0x100803}, {0xf9e8dc,0xf2c9ab,0xf7e8de,0x322114,78,66,0x3e2710,24,0x3e2710,13,0x4b2c09,8,0x4b2c09,14,0x2d1f11}, {0x231910,0x302217,0x1b130b,0x433325,0xf2ebe5,0xaf9f92}, {0xfaf7f5,0xffffff,0xf4eeea,0xe6ddd6,0x35271c,0x78685b}},
};
// DESIGN.md "Derived tokens": hue, swatch, bright, its hover, pressed and
// text-on, deep, its hover and pressed.
struct AccentVector { uint32_t swatch, bright, hover, pressed, on, deep, deepHover, deepPressed; };
static const AccentVector accents[BROOK_HUE_COUNT] = {
    {0x3f8cff,0x6aaeff,0x7fb9ff,0x5b96db,0x101e2e,0x1c65cc,0x1959b4,0x154d9b},
    {0x17409c,0x739fff,0x87acff,0x6389db,0x111c2e,0x17409c,0x143889,0x113177},
    {0x1fb5a8,0x4fd3c4,0x68d9cc,0x44b5a9,0x0d2227,0x09747e,0x08666f,0x075860},
    {0x2fa56f,0x62d6a0,0x78dcad,0x54b88a,0x0f2223,0x1b7650,0x186846,0x155a3d},
    {0x35d45f,0x7dff9b,0x8fffa9,0x6cdb85,0x132722,0x0f7a33,0x0d6b2d,0x0b5d27},
    {0x9acb2b,0xb4e04a,0xbee463,0x9bc140,0x192419,0x53710c,0x49630b,0x3f5609},
    {0xffc233,0xffc233,0xffcb50,0xdba72c,0x222016,0x8f5d00,0x7e5200,0x6d4700},
    {0xff7a1a,0xff8a3c,0xff9a57,0xdb7734,0x221917,0xb54600,0x9f3e00,0x8a3500},
    {0xf0392f,0xff625a,0xff7871,0xdb544d,0x22151b,0xc9211d,0xb11d1a,0x991916},
    {0xf0389b,0xff6cba,0xff81c4,0xdb5da0,0x221626,0xc71d7b,0xaf1a6c,0x97165d},
    {0x8d63f0,0xb79cff,0xc1aaff,0x9d86db,0x191c2e,0x6a43d6,0x5d3bbc,0x5133a3},
    {0x7088a8,0xa9bdd6,0xb5c6dc,0x91a3b8,0x181f2a,0x47607f,0x3e5470,0x364961},
    {0xf6b98a,0xf6b98a,0xf7c39a,0xd49f77,0x211f20,0xa8511d,0x94471a,0x803e16},
};
// DESIGN.md "Computed contrast", floored to one decimal: ink, secondary and
// tertiary ink (worse of glass tint and flat fill), ink on the title top,
// accent on chrome, accent line on the title top; dark then light chrome.
static const double contrast[BROOK_HUE_COUNT][2][6] = {
    {{13.3,7.9,4.6,7.2,6.4,3.4},{12.7,6.7,4.6,10.1,4.6,3.6}},
    {{15.4,8.7,4.8,9.4,6.6,4.0},{12.8,6.7,4.6,10.1,7.7,6.1}},
    {{13.0,7.8,4.5,6.9,7.8,4.1},{12.5,6.6,4.6,10.3,4.6,3.7}},
    {{13.0,7.7,4.5,6.8,7.9,4.1},{12.6,6.6,4.6,10.3,4.6,3.8}},
    {{13.0,7.7,4.5,6.8,11.3,6.0},{12.5,6.6,4.6,10.2,4.5,3.7}},
    {{13.1,7.8,4.6,6.9,9.4,5.0},{12.5,6.6,4.5,10.2,4.7,3.8}},
    {{11.2,6.9,4.6,4.5,7.6,3.1},{12.7,6.6,4.6,10.1,4.6,3.7}},
    {{11.4,6.9,4.6,6.3,5.4,3.0},{12.7,6.7,4.6,9.9,4.5,3.5}},
    {{12.5,7.1,4.5,7.9,4.7,3.0},{12.8,6.8,4.7,10.0,4.6,3.6}},
    {{13.8,8.0,4.6,7.8,5.9,3.3},{13.0,6.8,4.7,9.9,4.5,3.4}},
    {{13.6,8.0,4.7,7.8,6.6,3.8},{12.7,6.6,4.6,10.0,5.1,4.0}},
    {{13.2,7.8,4.6,7.2,7.6,4.1},{12.6,6.6,4.6,10.1,5.3,4.2}},
    {{13.5,7.9,4.6,7.4,8.7,4.8},{12.8,6.7,4.6,10.0,4.5,3.5}},
};

static unsigned near = 0, exact = 0;
static void Near(const char* hue, const char* what, uint32_t got, uint32_t want) {
    int worst = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        const int d = std::abs(int((got >> shift) & 255) - int((want >> shift) & 255));
        if (d > worst) worst = d;
    }
    if (worst > 1) { std::fprintf(stderr, "%s %s: %06x, want %06x\n", hue, what, got, want); ++failures; }
    (worst ? near : exact)++;
}
static double Floor1(double v) { return std::floor(v * 10) / 10; }
static double Worse(uint32_t ink, unsigned percent, uint32_t a, uint32_t b) {
    const double x = brook_contrast(brook_mix(a, ink, percent), a), y = brook_contrast(brook_mix(b, ink, percent), b);
    return x < y ? x : y;
}

int main() {
    // The libm-free maths against libm.
    for (double x = 1e-6; x < 3; x *= 1.37) {
        CHECK(std::fabs(brook_th_ln(x) - std::log(x)) < 1e-12);
        CHECK(std::fabs(brook_th_pow(x, 2.4) - std::pow(x, 2.4)) < 1e-12 * std::pow(x, 2.4) + 1e-15);
        CHECK(std::fabs(brook_th_cbrt(x) - std::cbrt(x)) < 1e-12);
        CHECK(std::fabs(brook_th_cbrt(-x) + std::cbrt(x)) < 1e-12);
    }
    for (unsigned c = 0; c < 256; ++c) CHECK(brook_linear_to_srgb(brook_srgb_to_linear(c)) == c);

    for (unsigned hue = 0; hue < BROOK_HUE_COUNT; ++hue) {
        BrookHueTokens t;
        brook_hue_derive(hue, &t);
        const Vector& v = vectors[hue];
        const char* k = brook_hue_key(hue);
        CHECK(!std::strcmp(k, v.key));
        const BrookChromeTokens& d = t.chrome[1];
        const BrookChromeTokens& l = t.chrome[0];
        Near(k, "dark glass", d.glass, v.dark.glass); Near(k, "dark title top", d.titleTop, v.dark.title);
        Near(k, "dark flat fill", d.flat, v.dark.flat); Near(k, "dark ink", d.ink, v.dark.ink);
        Near(k, "dark shadow", d.shadow, v.dark.shadow);
        CHECK(d.ink2 == v.dark.ink2 && d.ink3 == v.dark.ink3);
        Near(k, "light glass", l.glass, v.light.glass); Near(k, "light title top", l.titleTop, v.light.title);
        Near(k, "light flat fill", l.flat, v.light.flat); Near(k, "light ink", l.ink, v.light.ink);
        Near(k, "light hairline", l.line.rgb, v.light.line); Near(k, "light divider", l.divider.rgb, v.light.divider);
        Near(k, "light hover", l.hover.rgb, v.light.hover); Near(k, "light pressed", l.pressed.rgb, v.light.pressed);
        Near(k, "light shadow", l.shadow, v.light.shadow);
        CHECK(l.ink2 == v.light.ink2 && l.ink3 == v.light.ink3);
        CHECK(l.line.percent == v.light.lineA && l.divider.percent == v.light.dividerA &&
              l.hover.percent == v.light.hoverA && l.pressed.percent == v.light.pressedA);
        CHECK(d.line.rgb == 0xffffff && d.line.percent == 14 && d.divider.rgb == 0 && d.divider.percent == 28 &&
              d.hover.rgb == 0xffffff && d.hover.percent == 10 && d.pressed.percent == 17);
        const BrookContentTokens& cd = t.content[1];
        const BrookContentTokens& cl = t.content[0];
        Near(k, "dark app background", cd.background, v.contentDark.bg); Near(k, "dark app raised", cd.raised, v.contentDark.raised);
        Near(k, "dark app sunken", cd.sunken, v.contentDark.sunken); Near(k, "dark app line", cd.line, v.contentDark.line);
        Near(k, "dark app ink", cd.ink, v.contentDark.ink); Near(k, "dark app dim", cd.dim, v.contentDark.dim);
        Near(k, "light app background", cl.background, v.contentLight.bg); Near(k, "light app raised", cl.raised, v.contentLight.raised);
        Near(k, "light app sunken", cl.sunken, v.contentLight.sunken); Near(k, "light app line", cl.line, v.contentLight.line);
        Near(k, "light app ink", cl.ink, v.contentLight.ink); Near(k, "light app dim", cl.dim, v.contentLight.dim);
        // Accents and their derived tokens, exactly.
        const AccentVector& a = accents[hue];
        CHECK(t.swatch == a.swatch && d.accent == a.bright && d.accentHover == a.hover && d.accentPressed == a.pressed &&
              d.onAccent == a.on && l.accent == a.deep && l.accentHover == a.deepHover && l.accentPressed == a.deepPressed &&
              l.onAccent == 0xffffff);
        CHECK(cd.accent == a.bright && cl.accent == a.deep && cd.selection.percent == 16 && cl.selection.percent == 11);
        CHECK(d.rim.rgb == a.bright && d.rim.percent == 62 && l.rim.rgb == a.deep && l.rim.percent == 60);
        // HUD COMP: blue when the accent is green or teal (DESIGN.md "HUD colours").
        const bool green = hue == BROOK_HUE_TEAL || hue == BROOK_HUE_PINE || hue == BROOK_HUE_PHOSPHOR || hue == BROOK_HUE_LIME;
        CHECK(d.comp == (green ? 0x7db8ffu : 0x5fd6c8u) && l.comp == (green ? 0x1f6fe0u : 0x0a7f8au));
        // Computed contrast: within 0.1 of the DESIGN.md figure, and every
        // target holds (4.5 for text, 3.0 for the accent line).
        for (int dark = 1; dark >= 0; --dark) {
            const BrookChromeTokens& c = t.chrome[dark];
            const double got[6] = {
                Worse(c.ink, 100, c.glass, c.flat), Worse(c.ink, c.ink2, c.glass, c.flat), Worse(c.ink, c.ink3, c.glass, c.flat),
                brook_contrast(c.ink, c.titleTop),
                brook_contrast(c.accent, c.glass) < brook_contrast(c.accent, c.flat) ? brook_contrast(c.accent, c.glass)
                                                                                     : brook_contrast(c.accent, c.flat),
                brook_contrast(c.accent, c.titleTop)};
            for (int i = 0; i < 6; ++i) {
                const double want = contrast[hue][dark ? 0 : 1][i];
                if (std::fabs(Floor1(got[i]) - want) > 0.11) {
                    std::fprintf(stderr, "%s %s contrast %d: %.2f, DESIGN.md %.1f\n", k, dark ? "dark" : "light", i, got[i], want);
                    ++failures;
                }
                CHECK(got[i] >= (i == 5 ? 3.0 : 4.5));
            }
            for (uint32_t signal : {c.ok, c.warm, c.hot}) {
                CHECK(brook_contrast(signal, c.glass) >= 4.5 && brook_contrast(signal, c.flat) >= 4.5);
            }
            CHECK(brook_contrast(c.onAccent, c.accent) >= 4.5);
        }
        for (int dark = 0; dark < 2; ++dark) {
            const BrookContentTokens& c = t.content[dark];
            for (uint32_t s : {c.background, c.raised, c.sunken}) CHECK(brook_contrast(c.ink, s) >= 4.5);
            for (uint32_t s : {c.background, c.sunken}) CHECK(brook_contrast(c.dim, s) >= 4.5 && brook_contrast(c.accent, s) >= 4.5);
        }
        // Painter palettes: each theme's surfaces are the tokens; the Opaque
        // themes use the flat fill everywhere.
        for (unsigned theme = 0; theme < BROOK_THEME_COUNT; ++theme)
            for (int dark = 0; dark < 2; ++dark) {
                BrookThemePalette p;
                brook_theme_build_palette(&t, theme, dark, &p);
                const BrookChromeTokens& c = t.chrome[dark];
                if (theme == BROOK_THEME_GLASS) CHECK(p.titleActive == c.titleTop && p.titleInactive == c.glass && p.taskbar == c.glass);
                else CHECK(p.titleActive == c.flat && p.titleInactive == c.flat && p.taskbar == c.flat && p.launcher == c.flat);
                CHECK(p.text == c.ink && p.accent == c.accent && p.closeHover == BROOK_CLOSE_HOVER);
                CHECK(brook_contrast(p.secondaryText, p.taskbar) >= 4.5 && brook_contrast(p.mutedText, p.taskbar) >= 4.5);
            }
    }
    std::printf("hue tokens: %u exact, %u within one level\n", exact, near);
    CHECK(near <= 12);

    // Blue reproduces the reference exactly (the default has not moved).
    {
        BrookHueTokens t;
        brook_hue_derive(BROOK_HUE_BLUE, &t);
        CHECK(t.chrome[1].glass == BROOK_REF_DARK_GLASS && t.chrome[1].titleTop == BROOK_REF_DARK_TITLE &&
              t.chrome[1].flat == BROOK_REF_DARK_FLAT && t.chrome[0].glass == BROOK_REF_LIGHT_GLASS &&
              t.content[1].background == BROOK_REF_DARK_BG && t.content[0].background == BROOK_REF_LIGHT_BG);
    }

    for (unsigned material = 0; material < BROOK_THEME_COUNT; ++material)
        for (unsigned mode = 0; mode < BROOK_MODE_COUNT; ++mode) {
            CHECK(brook_theme_resolve(material, 1) == material);
            CHECK(brook_theme_resolve(material, 0) == BROOK_THEME_OPAQUE);
            CHECK(brook_theme_chrome_dark(mode) == (mode != BROOK_MODE_LIGHT));
            CHECK(brook_theme_content_dark(mode) == (mode == BROOK_MODE_DARK));
        }
    CHECK(brook_selection_valid(BROOK_THEME_SELECTION_DEFAULT) && BROOK_THEME_SELECTION_DEFAULT == 0x10u);
    CHECK(!brook_selection_valid(BROOK_THEME_SELECTION(2, 0, 0)) && !brook_selection_valid(BROOK_THEME_SELECTION(0, 3, 0)) &&
          !brook_selection_valid(BROOK_THEME_SELECTION(0, 0, 14)) && !brook_selection_valid(1u << 13));
    CHECK(!brook_selection_valid(brook_selection_make(BROOK_THEME_OPAQUE, BROOK_MODE_DARK, BROOK_HUE_BLUE, 1)));
    uint64_t selection;
    CHECK(brook_selection_from_legacy(BROOK_THEME_SELECTION(1, 1, 0), &selection) &&
          selection == BROOK_THEME_SELECTION(BROOK_THEME_OPAQUE, BROOK_MODE_LIGHT, BROOK_HUE_BLUE));
    CHECK(brook_selection_from_legacy(BROOK_THEME_SELECTION(2, 1, 0), &selection) &&
          selection == BROOK_THEME_SELECTION(BROOK_THEME_OPAQUE, BROOK_MODE_DARK, BROOK_HUE_BLUE));
    CHECK(!brook_selection_from_legacy(UINT64_C(1) << 32, &selection));
    CHECK(brook_selection_legacy(BROOK_THEME_SELECTION(BROOK_THEME_OPAQUE, BROOK_MODE_MIXED, BROOK_HUE_BLUE)) == 0x12);

    for (uint32_t rgb : {0x000000u, 0xffffffu, 0xff00ffu, 0x00ff00u, 0x777777u, 0x12112bu}) {
        const uint64_t custom = brook_selection_make(BROOK_THEME_OPAQUE, BROOK_MODE_MIXED, BROOK_HUE_CUSTOM, rgb);
        CHECK(brook_selection_valid(custom) && brook_selection_rgb(custom) == rgb);
        BrookHueTokens t;
        brook_hue_derive_selection(custom, &t);
        CHECK(t.swatch == rgb);
        for (unsigned i = 0; i < 2; ++i) {
            const auto& c = t.chrome[i];
            CHECK(c.accent == rgb && t.content[i].accent == rgb);
            for (uint32_t background : {c.glass, c.flat, c.titleTop}) CHECK(brook_contrast(c.ink, background) >= 4.5);
            for (uint32_t background : {c.accent, c.accentHover, c.accentPressed})
                CHECK(brook_contrast(c.onAccent, background) >= 4.5);
            if (rgb == 0xff00ff) {
                for (uint32_t tint : {c.glass, c.flat, c.titleTop})
                    CHECK(((tint >> 16) & 255) > ((tint >> 8) & 255) && (tint & 255) > ((tint >> 8) & 255));
            }
        }
    }
    BrookHueTokens pink;
    brook_hue_derive(BROOK_HUE_PINK, &pink);
    for (const auto& c : pink.chrome)
        for (uint32_t tint : {c.glass, c.flat, c.titleTop})
            CHECK(((tint >> 16) & 255) > ((tint >> 8) & 255) && (tint & 255) > ((tint >> 8) & 255));

    // settings.ini: named keys and the aliases (DESIGN.md "Native notes").
    auto parse = [](std::initializer_list<const char*> lines) {
        BrookThemeSettings s = brook_theme_settings_init();
        for (const char* line : lines) brook_theme_settings_line(&s, line);
        return brook_theme_settings_selection(&s);
    };
    CHECK(parse({}) == BROOK_THEME_SELECTION_DEFAULT);
    CHECK(parse({"theme=opaque-light", "mode=dark", "accent=amber"}) == BROOK_THEME_SELECTION(1, 0, BROOK_HUE_AMBER));
    CHECK(parse({"accent=wallpaper ", "theme=glass\r", "mode=light"}) == BROOK_THEME_SELECTION(0, 0, BROOK_HUE_WALLPAPER));
    CHECK(parse({"palette=tinted"}) == BROOK_THEME_SELECTION(0, 1, 0));
    CHECK(parse({"palette=clearwater", "dark=1"}) == BROOK_THEME_SELECTION(0, 2, 0));
    CHECK(parse({"palette=smoked"}) == BROOK_THEME_SELECTION(0, 1, BROOK_HUE_SLATE));
    CHECK(parse({"palette=opal", "dark=0"}) == BROOK_THEME_SELECTION(0, 0, BROOK_HUE_PINE));
    CHECK(parse({"theme=1", "dark=1"}) == BROOK_THEME_SELECTION(1, 0, BROOK_HUE_PINE));
    CHECK(parse({"theme=2", "dark=0", "performance=1"}) == BROOK_THEME_SELECTION(1, 2, BROOK_HUE_SLATE));
    CHECK(parse({"theme=0", "dark=1", "performance=1"}) == BROOK_THEME_SELECTION(1, 2, 0));
    CHECK(parse({"performance=1"}) == BROOK_THEME_SELECTION(1, 1, 0));
    CHECK(parse({"theme=opaque", "accent=#fF00Ff ", "mode=mixed"}) ==
          brook_selection_make(BROOK_THEME_OPAQUE, BROOK_MODE_MIXED, BROOK_HUE_CUSTOM, 0xff00ff));
    CHECK(parse({"accent=#000000", "theme=glass", "mode=dark"}) ==
          brook_selection_make(BROOK_THEME_GLASS, BROOK_MODE_DARK, BROOK_HUE_CUSTOM, 0));
    CHECK(parse({"accent=#ff00f"}) == BROOK_THEME_SELECTION_DEFAULT);
    CHECK(parse({"accent=#ff00ff00"}) == BROOK_THEME_SELECTION_DEFAULT);
    CHECK(parse({"accent=#ff00ff", "accent=blue"}) == BROOK_THEME_SELECTION_DEFAULT);
    char hex[8];
    brook_custom_accent_key(0xff00ff, hex);
    CHECK(!std::strcmp(hex, "#ff00ff"));
    CHECK(parse({"performance=1", "theme=glass", "dark=1", "mode=mixed"}) == BROOK_THEME_SELECTION(0, 1, 0)); // named keys win
    CHECK(parse({"accent=slate", "palette=opal"}) == BROOK_THEME_SELECTION(0, 1, BROOK_HUE_SLATE));
    CHECK(parse({"theme=sparkly", "mode=dim", "accent=chartreuse", "dark=2"}) == BROOK_THEME_SELECTION_DEFAULT);
    CHECK(parse({"theme=glassy", "mode=darker", "accent=bluegreen"}) == BROOK_THEME_SELECTION_DEFAULT);
    {
        BrookThemeSettings s = brook_theme_settings_init();
        CHECK(brook_theme_settings_line(&s, "opacity=80") == 0 && brook_theme_settings_line(&s, "scale=200") == 0);
    }
    for (unsigned i = 0; i < BROOK_LEGACY_PALETTE_COUNT; ++i) CHECK(brook_hue_legacy_palette(brook_legacy_palette_hue(i)) == i);
    if (failures) { std::fprintf(stderr, "BROOK_THEME: %d failures\n", failures); return 1; }
    std::puts("BROOK_THEME PASS");
}

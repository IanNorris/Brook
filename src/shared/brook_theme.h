#ifndef BROOK_THEME_H
#define BROOK_THEME_H
#include <stdint.h>

/* The one source of desktop colours, shared by the kernel chrome painters,
 * Settings, GTK apps (content colours) and native apps. C and C++,
 * freestanding (no libm). Packed colours are 0x00RRGGBB.
 *
 * Clearwater (docs/design/clearwater-next/DESIGN.md, "Hue and accent
 * colours"; compositor refactor stages 2.2 and 2.3):
 *   - three themes: Glass (Light, Mixed or Dark mode), Opaque Light and
 *     Opaque Dark;
 *   - thirteen hues; each turns the approved Blue reference set into a full
 *     token set by an OKLCH rotation, shaping, a gamut clip and a contrast
 *     loop (brook_hue_derive). The DESIGN.md tables are its host test
 *     vectors (test_brook_theme);
 *   - Glass needs a backend that can blur its backdrop. Without one it
 *     resolves to its Opaque stand-in (brook_theme_resolve); the saved
 *     selection is never rewritten. */

/* Theme-independent chrome colours (artwork and fallbacks). */
#define BROOK_CHROME_SHADE        0x00071526u /* material shading and shadow tint */
#define BROOK_WALLPAPER_FALLBACK  0x00001A3Au /* backdrop where no wallpaper covers */
#define BROOK_CURSOR_BUSY_IDLE    0x0053677Bu /* busy-ring dots other than the lit one */
#define BROOK_AGENT_ICON_EYE      0x004CD38Au /* taskbar agent indicator: present */
#define BROOK_AGENT_ICON_BADGE    0x00F0A020u /* taskbar agent indicator: stale badge */
#define BROOK_AGENT_ICON_MARK     0x001A1A1Au /* ... and the mark on it */
#define BROOK_CLOSE_HOVER         0x00C8414Fu /* the red Close hover, in every theme and hue */

enum {
    BROOK_THEME_GLASS = 0,
    BROOK_THEME_OPAQUE_LIGHT = 1,
    BROOK_THEME_OPAQUE_DARK = 2,
    BROOK_THEME_COUNT = 3
};
/* Glass only: Light glass and light apps, Mixed (dark glass around light
 * apps; the default) or Dark. An Opaque theme is one mode throughout. */
enum {
    BROOK_MODE_LIGHT = 0,
    BROOK_MODE_MIXED = 1,
    BROOK_MODE_DARK = 2,
    BROOK_MODE_COUNT = 3
};
enum {
    BROOK_HUE_BLUE = 0, BROOK_HUE_NAVY, BROOK_HUE_TEAL, BROOK_HUE_PINE, BROOK_HUE_PHOSPHOR,
    BROOK_HUE_LIME, BROOK_HUE_AMBER, BROOK_HUE_ORANGE, BROOK_HUE_RED, BROOK_HUE_PINK,
    BROOK_HUE_VIOLET, BROOK_HUE_SLATE, BROOK_HUE_WALLPAPER,
    BROOK_HUE_COUNT
};
/* The palettes before Clearwater, as numbered by settings.ini theme=0..2 and
 * 0xB02 ops 0/1. They survive as hues: Smoked is Slate, Opal is Pine green. */
enum {
    BROOK_LEGACY_PALETTE_CLEARWATER = 0,
    BROOK_LEGACY_PALETTE_OPAL = 1,
    BROOK_LEGACY_PALETTE_SMOKED = 2,
    BROOK_LEGACY_PALETTE_COUNT = 3
};
static inline unsigned brook_legacy_palette_hue(unsigned id) {
    return id == BROOK_LEGACY_PALETTE_OPAL ? (unsigned)BROOK_HUE_PINE :
           id == BROOK_LEGACY_PALETTE_SMOKED ? (unsigned)BROOK_HUE_SLATE : (unsigned)BROOK_HUE_BLUE;
}
static inline unsigned brook_hue_legacy_palette(unsigned hue) {
    return hue == BROOK_HUE_PINE ? (unsigned)BROOK_LEGACY_PALETTE_OPAL :
           hue == BROOK_HUE_SLATE ? (unsigned)BROOK_LEGACY_PALETTE_SMOKED : (unsigned)BROOK_LEGACY_PALETTE_CLEARWATER;
}

/* The selection word (0xB02 ops 2/3): theme in bits 0-3, mode in bits 4-7,
 * hue in bits 8-12; every other bit is zero. */
#define BROOK_THEME_SELECTION(theme, mode, hue) \
    ((uint32_t)(theme) | (uint32_t)(mode) << 4 | (uint32_t)(hue) << 8)
#define BROOK_THEME_SELECTION_DEFAULT BROOK_THEME_SELECTION(BROOK_THEME_GLASS, BROOK_MODE_MIXED, BROOK_HUE_BLUE)
static inline unsigned brook_selection_theme(uint32_t s) { return s & 15u; }
static inline unsigned brook_selection_mode(uint32_t s) { return (s >> 4) & 15u; }
static inline unsigned brook_selection_hue(uint32_t s) { return (s >> 8) & 31u; }
static inline int brook_selection_valid(uint32_t s) {
    return !(s & ~0x1fffu) && brook_selection_theme(s) < BROOK_THEME_COUNT &&
           brook_selection_mode(s) < BROOK_MODE_COUNT && brook_selection_hue(s) < BROOK_HUE_COUNT;
}
/* 0xB02 ops. 0/1: the legacy palette id (query; set = choose its hue).
 * 2/3: the selection word (query; set). 4: status (brook_appearance.h). */
enum {
    BROOK_THEME_OP_LEGACY_QUERY = 0,
    BROOK_THEME_OP_LEGACY_SET = 1,
    BROOK_THEME_OP_QUERY = 2,
    BROOK_THEME_OP_SET = 3
};

/* Glass resolves to its Opaque stand-in when the backend cannot blur:
 * Opaque Light if the mode was Light, otherwise Opaque Dark. */
static inline unsigned brook_theme_resolve(unsigned theme, unsigned mode, int glassAvailable) {
    if (theme == BROOK_THEME_GLASS && !glassAvailable)
        return mode == BROOK_MODE_LIGHT ? (unsigned)BROOK_THEME_OPAQUE_LIGHT : (unsigned)BROOK_THEME_OPAQUE_DARK;
    return theme < BROOK_THEME_COUNT ? theme : (unsigned)BROOK_THEME_GLASS;
}
/* For a resolved theme: dark chrome, and dark app content. Mixed is dark
 * glass with light apps. */
static inline int brook_theme_chrome_dark(unsigned resolved, unsigned mode) {
    return resolved == BROOK_THEME_GLASS ? mode != BROOK_MODE_LIGHT : resolved == BROOK_THEME_OPAQUE_DARK;
}
static inline int brook_theme_content_dark(unsigned resolved, unsigned mode) {
    return resolved == BROOK_THEME_GLASS ? mode == BROOK_MODE_DARK : resolved == BROOK_THEME_OPAQUE_DARK;
}

static inline const char* brook_theme_key(unsigned theme) {
    static const char* const keys[BROOK_THEME_COUNT] = {"glass", "opaque-light", "opaque-dark"};
    return theme < BROOK_THEME_COUNT ? keys[theme] : "glass";
}
static inline const char* brook_theme_name(unsigned theme) {
    static const char* const names[BROOK_THEME_COUNT] = {"Glass", "Opaque Light", "Opaque Dark"};
    return theme < BROOK_THEME_COUNT ? names[theme] : "Glass";
}
static inline const char* brook_mode_key(unsigned mode) {
    static const char* const keys[BROOK_MODE_COUNT] = {"light", "mixed", "dark"};
    return mode < BROOK_MODE_COUNT ? keys[mode] : "mixed";
}
static inline const char* brook_mode_name(unsigned mode) {
    static const char* const names[BROOK_MODE_COUNT] = {"Light", "Mixed", "Dark"};
    return mode < BROOK_MODE_COUNT ? names[mode] : "Mixed";
}
static inline const char* brook_hue_key(unsigned hue) {
    static const char* const keys[BROOK_HUE_COUNT] = {"blue", "navy", "teal", "pine", "phosphor", "lime", "amber",
                                                      "orange", "red", "pink", "violet", "slate", "wallpaper"};
    return hue < BROOK_HUE_COUNT ? keys[hue] : "blue";
}
static inline const char* brook_hue_name(unsigned hue) {
    static const char* const names[BROOK_HUE_COUNT] = {"Blue", "Navy", "Teal", "Pine green", "Phosphor", "Lime",
                                                       "Amber", "Orange", "Red", "Hot pink", "Violet", "Slate",
                                                       "Wallpaper"};
    return hue < BROOK_HUE_COUNT ? names[hue] : "Blue";
}

/* ------------------------------------------------------------------------
 * settings.ini [Desktop]: theme=glass|opaque-light|opaque-dark, mode=light|
 * mixed|dark, accent=<hue key>. Older values are aliases: theme=0|1|2 (the
 * legacy palettes: Clearwater, Opal = Pine green, Smoked = Slate),
 * palette=tinted|clearwater (Glass), palette=smoked|opal (Glass with Slate or
 * Pine green), dark=0|1 (the mode when mode= is absent) and performance=1
 * (the Opaque theme for that mode when theme= names none). Named keys win
 * whatever the order. Malformed values are ignored.
 * ---------------------------------------------------------------------- */
typedef struct BrookThemeSettings {
    int theme, mode, hue;          /* named keys; -1 unset */
    int legacyPalette, paletteHue; /* theme=0..2; palette= (-1 unset; -2 the default hue) */
    int dark, performance;         /* -1 unset */
} BrookThemeSettings;
static inline BrookThemeSettings brook_theme_settings_init(void) {
    BrookThemeSettings s = {-1, -1, -1, -1, -1, -1, -1};
    return s;
}
/* The value after "key=", trailing blanks dropped, equals word. */
static inline int brook_theme_value_is(const char* value, const char* word) {
    while (*word && *value == *word) { ++value; ++word; }
    if (*word) return 0;
    while (*value == ' ' || *value == '\t' || *value == '\r') ++value;
    return !*value;
}
static inline const char* brook_theme_after_key(const char* line, const char* key) {
    while (*key && *line == *key) { ++line; ++key; }
    return *key ? 0 : line;
}
/* One [Desktop] line; returns nonzero if it was a theme key. */
static inline int brook_theme_settings_line(BrookThemeSettings* s, const char* line) {
    const char* v;
    unsigned i;
    if ((v = brook_theme_after_key(line, "theme="))) {
        for (i = 0; i < BROOK_THEME_COUNT; ++i)
            if (brook_theme_value_is(v, brook_theme_key(i))) { s->theme = (int)i; return 1; }
        for (i = 0; i < BROOK_LEGACY_PALETTE_COUNT; ++i) {
            const char digit[2] = {(char)('0' + i), 0};
            if (brook_theme_value_is(v, digit)) { s->legacyPalette = (int)i; return 1; }
        }
        return 1;
    }
    if ((v = brook_theme_after_key(line, "mode="))) {
        for (i = 0; i < BROOK_MODE_COUNT; ++i)
            if (brook_theme_value_is(v, brook_mode_key(i))) { s->mode = (int)i; return 1; }
        return 1;
    }
    if ((v = brook_theme_after_key(line, "accent="))) {
        for (i = 0; i < BROOK_HUE_COUNT; ++i)
            if (brook_theme_value_is(v, brook_hue_key(i))) { s->hue = (int)i; return 1; }
        return 1;
    }
    if ((v = brook_theme_after_key(line, "palette="))) {
        if (brook_theme_value_is(v, "tinted") || brook_theme_value_is(v, "clearwater")) s->paletteHue = -2;
        else if (brook_theme_value_is(v, "smoked")) s->paletteHue = BROOK_HUE_SLATE;
        else if (brook_theme_value_is(v, "opal")) s->paletteHue = BROOK_HUE_PINE;
        return 1;
    }
    if ((v = brook_theme_after_key(line, "dark="))) {
        if (brook_theme_value_is(v, "0") || brook_theme_value_is(v, "1")) s->dark = *v == '1';
        return 1;
    }
    if ((v = brook_theme_after_key(line, "performance="))) {
        if (brook_theme_value_is(v, "0") || brook_theme_value_is(v, "1")) s->performance = *v == '1';
        return 1;
    }
    return 0;
}
static inline uint32_t brook_theme_settings_selection(const BrookThemeSettings* s) {
    const unsigned mode = s->mode >= 0 ? (unsigned)s->mode :
                          s->dark >= 0 ? (s->dark ? (unsigned)BROOK_MODE_DARK : (unsigned)BROOK_MODE_LIGHT) :
                          (unsigned)BROOK_MODE_MIXED;
    const unsigned theme = s->theme >= 0 ? (unsigned)s->theme :
                           s->performance == 1 ? (mode == BROOK_MODE_LIGHT ? (unsigned)BROOK_THEME_OPAQUE_LIGHT :
                                                                             (unsigned)BROOK_THEME_OPAQUE_DARK) :
                           (unsigned)BROOK_THEME_GLASS;
    const unsigned hue = s->hue >= 0 ? (unsigned)s->hue :
                         s->paletteHue >= 0 ? (unsigned)s->paletteHue :
                         s->paletteHue == -2 ? (unsigned)BROOK_HUE_BLUE :
                         s->legacyPalette >= 0 ? brook_legacy_palette_hue((unsigned)s->legacyPalette) :
                         (unsigned)BROOK_HUE_BLUE;
    return BROOK_THEME_SELECTION(theme, mode, hue);
}

/* ------------------------------------------------------------------------
 * Colour maths: sRGB, OKLab and WCAG 2 contrast, in double precision
 * without libm (the kernel links none). About thirty tokens per hue, once
 * per hue: never per frame.
 * ---------------------------------------------------------------------- */
static inline double brook_th_ln(double x) {
    uint64_t bits;
    int e;
    double m, z, z2, term, sum;
    unsigned k;
    if (!(x > 0)) return -1e300;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    e = (int)((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    __builtin_memcpy(&m, &bits, sizeof(m));
    if (m > 1.4142135623730951) { m *= 0.5; ++e; }
    z = (m - 1.0) / (m + 1.0);
    z2 = z * z;
    term = z;
    sum = 0;
    for (k = 1; k < 80; k += 2) {
        const double add = term / (double)k;
        sum += add;
        if (add < 1e-18 && add > -1e-18) break;
        term *= z2;
    }
    return 2.0 * sum + (double)e * 0.6931471805599453;
}
static inline double brook_th_exp(double x) {
    const double ln2 = 0.6931471805599453;
    double kf, r, sum, term, scale;
    long k;
    unsigned i;
    uint64_t bits;
    if (x < -700) return 0;
    if (x > 700) x = 700;
    kf = x / ln2;
    k = (long)(kf < 0 ? kf - 0.5 : kf + 0.5);
    r = x - (double)k * ln2;
    sum = 1;
    term = 1;
    for (i = 1; i < 40; ++i) {
        term *= r / (double)i;
        sum += term;
        if (term < 1e-18 && term > -1e-18) break;
    }
    bits = (uint64_t)(k + 1023) << 52;
    __builtin_memcpy(&scale, &bits, sizeof(scale));
    return sum * scale;
}
static inline double brook_th_pow(double x, double y) { return x > 0 ? brook_th_exp(y * brook_th_ln(x)) : 0; }
static inline double brook_th_cbrt(double x) {
    return x > 0 ? brook_th_pow(x, 1.0 / 3.0) : x < 0 ? -brook_th_pow(-x, 1.0 / 3.0) : 0;
}
static inline double brook_th_sqrt(double x) { return x > 0 ? brook_th_pow(x, 0.5) : 0; }
static inline long brook_th_round(double x) { return (long)(x < 0 ? x - 0.5 : x + 0.5); }

static inline double brook_srgb_to_linear(unsigned c) {
    const double v = (double)c / 255.0;
    return v <= 0.04045 ? v / 12.92 : brook_th_pow((v + 0.055) / 1.055, 2.4);
}
static inline unsigned brook_linear_to_srgb(double v) {
    double s;
    long n;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    s = v <= 0.0031308 ? 12.92 * v : 1.055 * brook_th_pow(v, 1.0 / 2.4) - 0.055;
    n = brook_th_round(s * 255.0);
    return n < 0 ? 0u : n > 255 ? 255u : (unsigned)n;
}
/* WCAG 2 relative luminance and contrast ratio of two opaque colours. */
static inline double brook_luminance(uint32_t rgb) {
    return 0.2126 * brook_srgb_to_linear((rgb >> 16) & 255) + 0.7152 * brook_srgb_to_linear((rgb >> 8) & 255) +
           0.0722 * brook_srgb_to_linear(rgb & 255);
}
static inline double brook_contrast(uint32_t a, uint32_t b) {
    const double la = brook_luminance(a), lb = brook_luminance(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}
/* fg at percent% over bg, in sRGB (CSS colour-mix, or rgb()/alpha over an
 * opaque surface), per channel in exact integer arithmetic, halves rounded
 * to even as the design's generator did. */
static inline uint32_t brook_mix(uint32_t bg, uint32_t fg, unsigned percent) {
    uint32_t out = 0;
    unsigned shift;
    if (percent > 100) percent = 100;
    for (shift = 0; shift < 24; shift += 8) {
        const unsigned v = ((fg >> shift) & 255) * percent + ((bg >> shift) & 255) * (100 - percent);
        unsigned q = v / 100;
        const unsigned rem = v % 100;
        if (rem > 50 || (rem == 50 && (q & 1))) ++q;
        out |= (uint32_t)q << shift;
    }
    return out;
}

typedef struct BrookOklab { double L, a, b; } BrookOklab;
static inline BrookOklab brook_oklab(uint32_t rgb) {
    const double r = brook_srgb_to_linear((rgb >> 16) & 255), g = brook_srgb_to_linear((rgb >> 8) & 255),
                 b = brook_srgb_to_linear(rgb & 255);
    const double l = brook_th_cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = brook_th_cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = brook_th_cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    BrookOklab o;
    o.L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    o.a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    o.b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return o;
}
static inline void brook_oklab_linear(double L, double a, double b, double out[3]) {
    double l = L + 0.3963377774 * a + 0.2158037573 * b;
    double m = L - 0.1055613458 * a - 0.0638541728 * b;
    double s = L - 0.0894841775 * a - 1.2914855480 * b;
    l = l * l * l; m = m * m * m; s = s * s * s;
    out[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    out[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    out[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}
static inline int brook_linear_in_gamut(const double v[3]) {
    return v[0] >= -1e-6 && v[0] <= 1 + 1e-6 && v[1] >= -1e-6 && v[1] <= 1 + 1e-6 && v[2] >= -1e-6 && v[2] <= 1 + 1e-6;
}
/* OKLCH (lightness L, chroma C along the unit hue direction (ca, sa)) to
 * sRGB, reducing chroma until the colour is inside sRGB (step 4). */
static inline uint32_t brook_oklch_srgb(double L, double C, double ca, double sa) {
    double v[3];
    brook_oklab_linear(L, C * ca, C * sa, v);
    if (!brook_linear_in_gamut(v)) {
        double lo = 0, hi = C;
        unsigned i;
        for (i = 0; i < 40; ++i) {
            const double mid = (lo + hi) / 2;
            brook_oklab_linear(L, mid * ca, mid * sa, v);
            if (brook_linear_in_gamut(v)) lo = mid; else hi = mid;
        }
        brook_oklab_linear(L, lo * ca, lo * sa, v);
    }
    return brook_linear_to_srgb(v[0]) << 16 | brook_linear_to_srgb(v[1]) << 8 | brook_linear_to_srgb(v[2]);
}

/* ------------------------------------------------------------------------
 * Hue tables (DESIGN.md "Derived tokens" and "How a hue becomes a token
 * set"). Each hue has a swatch (the picker only), a bright accent (dark
 * chrome and dark content) and a deep accent (light chrome and content).
 * ---------------------------------------------------------------------- */
typedef struct BrookHueAccent { uint32_t swatch, bright, deep; } BrookHueAccent;
static inline const BrookHueAccent* brook_hue_accent(unsigned hue) {
    static const BrookHueAccent accents[BROOK_HUE_COUNT] = {
        {0x3f8cffu, 0x6aaeffu, 0x1c65ccu}, /* Blue */
        {0x17409cu, 0x739fffu, 0x17409cu}, /* Navy */
        {0x1fb5a8u, 0x4fd3c4u, 0x09747eu}, /* Teal */
        {0x2fa56fu, 0x62d6a0u, 0x1b7650u}, /* Pine green */
        {0x35d45fu, 0x7dff9bu, 0x0f7a33u}, /* Phosphor */
        {0x9acb2bu, 0xb4e04au, 0x53710cu}, /* Lime */
        {0xffc233u, 0xffc233u, 0x8f5d00u}, /* Amber */
        {0xff7a1au, 0xff8a3cu, 0xb54600u}, /* Orange */
        {0xf0392fu, 0xff625au, 0xc9211du}, /* Red */
        {0xf0389bu, 0xff6cbau, 0xc71d7bu}, /* Hot pink */
        {0x8d63f0u, 0xb79cffu, 0x6a43d6u}, /* Violet */
        {0x7088a8u, 0xa9bdd6u, 0x47607fu}, /* Slate */
        {0xf6b98au, 0xf6b98au, 0xa8511du}, /* Wallpaper: sampled from brook-dawn.png */
    };
    return &accents[hue < BROOK_HUE_COUNT ? hue : 0];
}
/* Step 3 shaping: lightness offset and chroma factor for dark glass and the
 * flat fill, the dark title top, light chrome (chroma) and dark app
 * surfaces. A chroma factor of 3 means "as much as sRGB holds". */
typedef struct BrookHueShaping { double glassL, glassC, titleL, titleC, lightC, appL, appC; } BrookHueShaping;
static inline BrookHueShaping brook_hue_shaping(unsigned hue) {
    static const BrookHueShaping none = {0, 1, 0, 1, 1, 0, 1};
    static const BrookHueShaping amber = {0.075, 3, 0.11, 3, 2.2, 0, 1.25};
    static const BrookHueShaping orange = {0.05, 3, 0.06, 3, 1.7, 0, 1.2};
    static const BrookHueShaping red = {0.03, 3, 0.04, 3, 1.6, 0, 1.25};
    static const BrookHueShaping navy = {-0.045, 1.5, -0.05, 1.4, 1.25, -0.03, 1.3};
    return hue == BROOK_HUE_AMBER ? amber : hue == BROOK_HUE_ORANGE ? orange : hue == BROOK_HUE_RED ? red :
           hue == BROOK_HUE_NAVY ? navy : none;
}

/* A colour drawn at a fixed alpha over a surface (hairlines, hover fills). */
typedef struct BrookTint { uint32_t rgb; unsigned percent; } BrookTint;
static inline uint32_t brook_tint_over(BrookTint t, uint32_t surface) { return brook_mix(surface, t.rgb, t.percent); }

/* Chrome tokens of one chrome mode (DESIGN.md "Token sheet", "Chrome, by
 * mode"). In Glass the opacity setting supplies alpha to glass and titleTop;
 * the flat fill is always opaque. */
typedef struct BrookChromeTokens {
    uint32_t glass, titleTop, flat;          /* glass tint, active title top, flat fill (Opaque themes, menus) */
    uint32_t ink;                            /* text and icons */
    unsigned ink2, ink3;                     /* secondary and tertiary ink alpha, percent */
    BrookTint line, divider, hover, pressed; /* outer hairline, titlebar divider, hover and pressed fills */
    uint32_t shadow;
    uint32_t accent, accentHover, accentPressed, onAccent;
    BrookTint rim;                           /* active window rim */
    uint32_t ok, warm, hot;                  /* signal colours */
    uint32_t comp;                           /* HUD COMP, the second graph colour */
} BrookChromeTokens;
/* App surface tokens of one content mode ("Content, by mode"). */
typedef struct BrookContentTokens {
    uint32_t background, raised, sunken, line, ink, dim;
    uint32_t accent, accentHover, accentPressed, onAccent;
    BrookTint selection;
} BrookContentTokens;
/* Index 0: light, 1: dark. */
typedef struct BrookHueTokens {
    BrookChromeTokens chrome[2];
    BrookContentTokens content[2];
    uint32_t swatch;
} BrookHueTokens;

/* The approved Blue set: the reference every hue is derived from. */
enum {
    BROOK_REF_DARK_GLASS = 0x102442, BROOK_REF_DARK_TITLE = 0x28508a, BROOK_REF_DARK_FLAT = 0x162844,
    BROOK_REF_DARK_INK = 0xeef4fc, BROOK_REF_DARK_SHADOW = 0x030a16,
    BROOK_REF_LIGHT_GLASS = 0xe2edfb, BROOK_REF_LIGHT_TITLE = 0xbad5f8, BROOK_REF_LIGHT_FLAT = 0xe2ebf7,
    BROOK_REF_LIGHT_INK = 0x14263d, BROOK_REF_LIGHT_LINE = 0x142c50, BROOK_REF_LIGHT_HOVER = 0x143264,
    BROOK_REF_LIGHT_SHADOW = 0x142238,
    BROOK_REF_DARK_BG = 0x111c2b, BROOK_REF_DARK_RAISED = 0x18263a, BROOK_REF_DARK_SUNKEN = 0x0c1522,
    BROOK_REF_DARK_LINE = 0x26374f, BROOK_REF_DARK_CINK = 0xe6edf7, BROOK_REF_DARK_DIM = 0x93a4bb,
    BROOK_REF_LIGHT_BG = 0xf5f8fc, BROOK_REF_LIGHT_RAISED = 0xffffff, BROOK_REF_LIGHT_SUNKEN = 0xeaf0f8,
    BROOK_REF_LIGHT_CLINE = 0xd7e0ed, BROOK_REF_LIGHT_CINK = 0x1c2b3f, BROOK_REF_LIGHT_DIM = 0x5c6d84
};
/* Signal colours: bright on dark chrome, deepened on light chrome. */
enum {
    BROOK_OK_DARK = 0x5fcf8f, BROOK_WARM_DARK = 0xf0b35a, BROOK_HOT_DARK = 0xff6f6a,
    BROOK_OK_LIGHT = 0x157040, BROOK_WARM_LIGHT = 0x855400, BROOK_HOT_LIGHT = 0xc0271f
};

/* The rotation of the reference set onto one hue (steps 1 and 2): the hue
 * angle difference to Blue's accent as a rotation, and min(1, accent chroma
 * / Blue's chroma). */
typedef struct BrookHueRotation { double cosine, sine, chroma; } BrookHueRotation;
static inline BrookHueRotation brook_hue_rotation(uint32_t bright) {
    const BrookOklab ref = brook_oklab(0x6aaeffu), acc = brook_oklab(bright);
    const double c0 = brook_th_sqrt(ref.a * ref.a + ref.b * ref.b), c1 = brook_th_sqrt(acc.a * acc.a + acc.b * acc.b);
    BrookHueRotation r = {1, 0, 1};
    if (c1 > 1e-9) {
        r.cosine = (ref.a * acc.a + ref.b * acc.b) / (c0 * c1);
        r.sine = (ref.a * acc.b - ref.b * acc.a) / (c0 * c1);
    }
    r.chroma = c1 / c0 < 1 ? c1 / c0 : 1;
    return r;
}
/* A reference token rotated onto the hue, its lightness offset by dl and
 * its chroma scaled by cf (step 3), clipped to sRGB (step 4), then its
 * lightness stepped by 0.004 in direction step (-1 darker, +1 lighter) until
 * it passes every check (step 5). */
typedef struct BrookContrastCheck { uint32_t colour; double target; } BrookContrastCheck;
static inline uint32_t brook_hue_token(uint32_t reference, BrookHueRotation r, double dl, double cf,
                                       const BrookContrastCheck* checks, unsigned count, int step) {
    const BrookOklab o = brook_oklab(reference);
    const double c = brook_th_sqrt(o.a * o.a + o.b * o.b);
    double ca = 1, sa = 0, L = o.L + dl;
    unsigned i, n;
    uint32_t rgb;
    if (c > 1e-12) {
        const double a = o.a / c, b = o.b / c;
        ca = a * r.cosine - b * r.sine;
        sa = a * r.sine + b * r.cosine;
    }
    for (n = 0;; ++n) {
        int ok = 1;
        rgb = brook_oklch_srgb(L, c * r.chroma * cf, ca, sa);
        for (i = 0; i < count && ok; ++i) ok = brook_contrast(checks[i].colour, rgb) >= checks[i].target;
        if (ok || !step || n >= 200 || L <= 0 || L >= 1) return rgb;
        L += step * 0.004;
    }
}
/* The smallest alpha (from start, in steps of 2 points) at which ink over
 * every surface reaches 4.5. */
static inline unsigned brook_ink_alpha(uint32_t ink, unsigned start, const uint32_t* surfaces, unsigned count) {
    unsigned percent = start, i;
    for (; percent < 100; percent += 2) {
        int ok = 1;
        for (i = 0; i < count && ok; ++i) ok = brook_contrast(brook_mix(surfaces[i], ink, percent), surfaces[i]) >= 4.5;
        if (ok) break;
    }
    return percent > 100 ? 100 : percent;
}

/* Derive one hue's token set from its accent and shaping (none for a
 * sampled wallpaper hue). Blue reproduces the reference exactly. */
static inline void brook_hue_derive_from(const BrookHueAccent* accent, BrookHueShaping sh, BrookHueTokens* out) {
    const BrookHueRotation r = brook_hue_rotation(accent->bright);
    const uint32_t bright = accent->bright, deep = accent->deep;
    BrookChromeTokens* d = &out->chrome[1];
    BrookChromeTokens* l = &out->chrome[0];
    BrookContentTokens* cd = &out->content[1];
    BrookContentTokens* cl = &out->content[0];
    out->swatch = accent->swatch;
    /* Dark chrome: its surfaces carry ink, the accent and the signals at
     * 4.5; the title top carries ink at 4.5 and the accent line at 3.0. */
    d->ink = brook_hue_token(BROOK_REF_DARK_INK, r, 0, 1, 0, 0, 0);
    {
        const BrookContrastCheck surface[5] = {{d->ink, 4.5}, {bright, 4.5}, {BROOK_OK_DARK, 4.5},
                                               {BROOK_WARM_DARK, 4.5}, {BROOK_HOT_DARK, 4.5}};
        const BrookContrastCheck title[2] = {{d->ink, 4.5}, {bright, 3.0}};
        uint32_t both[2];
        d->glass = brook_hue_token(BROOK_REF_DARK_GLASS, r, sh.glassL, sh.glassC, surface, 5, -1);
        d->flat = brook_hue_token(BROOK_REF_DARK_FLAT, r, sh.glassL, sh.glassC, surface, 5, -1);
        d->titleTop = brook_hue_token(BROOK_REF_DARK_TITLE, r, sh.titleL, sh.titleC, title, 2, -1);
        both[0] = d->glass; both[1] = d->flat;
        d->ink2 = brook_ink_alpha(d->ink, 74, both, 2);
        d->ink3 = brook_ink_alpha(d->ink, 52, both, 2);
    }
    d->line.rgb = 0xffffffu; d->line.percent = 14;
    d->divider.rgb = 0x000000u; d->divider.percent = 28;
    d->hover.rgb = 0xffffffu; d->hover.percent = 10;
    d->pressed.rgb = 0xffffffu; d->pressed.percent = 17;
    d->shadow = brook_hue_token(BROOK_REF_DARK_SHADOW, r, 0, 1, 0, 0, 0);
    /* Light chrome: the same checks, lightening. */
    l->ink = brook_hue_token(BROOK_REF_LIGHT_INK, r, 0, 1, 0, 0, 0);
    {
        const BrookContrastCheck surface[5] = {{l->ink, 4.5}, {deep, 4.5}, {BROOK_OK_LIGHT, 4.5},
                                               {BROOK_WARM_LIGHT, 4.5}, {BROOK_HOT_LIGHT, 4.5}};
        const BrookContrastCheck title[2] = {{l->ink, 4.5}, {deep, 3.0}};
        uint32_t both[2];
        l->glass = brook_hue_token(BROOK_REF_LIGHT_GLASS, r, 0, sh.lightC, surface, 5, 1);
        l->flat = brook_hue_token(BROOK_REF_LIGHT_FLAT, r, 0, sh.lightC, surface, 5, 1);
        l->titleTop = brook_hue_token(BROOK_REF_LIGHT_TITLE, r, 0, sh.lightC, title, 2, 1);
        both[0] = l->glass; both[1] = l->flat;
        l->ink2 = brook_ink_alpha(l->ink, 78, both, 2);
        l->ink3 = brook_ink_alpha(l->ink, 66, both, 2);
    }
    l->line.rgb = l->divider.rgb = brook_hue_token(BROOK_REF_LIGHT_LINE, r, 0, 1, 0, 0, 0);
    l->line.percent = 24; l->divider.percent = 13;
    l->hover.rgb = l->pressed.rgb = brook_hue_token(BROOK_REF_LIGHT_HOVER, r, 0, 1, 0, 0, 0);
    l->hover.percent = 8; l->pressed.percent = 14;
    l->shadow = brook_hue_token(BROOK_REF_LIGHT_SHADOW, r, 0, 1, 0, 0, 0);
    /* Accents and their derived tokens ("Derived tokens"). */
    d->accent = bright;
    d->accentHover = brook_mix(bright, 0xffffffu, 14);
    d->accentPressed = brook_mix(bright, 0x000000u, 14);
    d->onAccent = brook_mix(bright, 0x040a12u, 88);
    d->rim.rgb = bright; d->rim.percent = 62;
    l->accent = deep;
    l->accentHover = brook_mix(deep, 0x000000u, 12);
    l->accentPressed = brook_mix(deep, 0x000000u, 24);
    l->onAccent = 0xffffffu;
    l->rim.rgb = deep; l->rim.percent = 60;
    d->ok = BROOK_OK_DARK; d->warm = BROOK_WARM_DARK; d->hot = BROOK_HOT_DARK;
    l->ok = BROOK_OK_LIGHT; l->warm = BROOK_WARM_LIGHT; l->hot = BROOK_HOT_LIGHT;
    {
        /* HUD COMP is teal, or blue when the accent is itself green or teal
         * (an OKLCH hue between 100 and 220 degrees). */
        const BrookOklab o = brook_oklab(bright);
        const double c = brook_th_sqrt(o.a * o.a + o.b * o.b);
        const int green = c > 1e-9 && o.a < -0.1736 * c && o.b > -0.6428 * c;
        d->comp = green ? 0x7db8ffu : 0x5fd6c8u;
        l->comp = green ? 0x1f6fe0u : 0x0a7f8au;
    }
    /* App surfaces. */
    cd->background = brook_hue_token(BROOK_REF_DARK_BG, r, sh.appL, sh.appC, 0, 0, 0);
    cd->raised = brook_hue_token(BROOK_REF_DARK_RAISED, r, sh.appL, sh.appC, 0, 0, 0);
    cd->sunken = brook_hue_token(BROOK_REF_DARK_SUNKEN, r, sh.appL, sh.appC, 0, 0, 0);
    cd->line = brook_hue_token(BROOK_REF_DARK_LINE, r, sh.appL, sh.appC, 0, 0, 0);
    cd->ink = brook_hue_token(BROOK_REF_DARK_CINK, r, 0, 1, 0, 0, 0);
    cd->dim = brook_hue_token(BROOK_REF_DARK_DIM, r, 0, 1, 0, 0, 0);
    cl->background = brook_hue_token(BROOK_REF_LIGHT_BG, r, 0, 1, 0, 0, 0);
    cl->raised = brook_hue_token(BROOK_REF_LIGHT_RAISED, r, 0, 1, 0, 0, 0);
    cl->sunken = brook_hue_token(BROOK_REF_LIGHT_SUNKEN, r, 0, 1, 0, 0, 0);
    cl->line = brook_hue_token(BROOK_REF_LIGHT_CLINE, r, 0, 1, 0, 0, 0);
    cl->ink = brook_hue_token(BROOK_REF_LIGHT_CINK, r, 0, 1, 0, 0, 0);
    cl->dim = brook_hue_token(BROOK_REF_LIGHT_DIM, r, 0, 1, 0, 0, 0);
    /* Content accents equal the chrome accent of the same lightness. */
    cd->accent = d->accent; cd->accentHover = d->accentHover; cd->accentPressed = d->accentPressed;
    cd->onAccent = d->onAccent; cd->selection.rgb = bright; cd->selection.percent = 16;
    cl->accent = l->accent; cl->accentHover = l->accentHover; cl->accentPressed = l->accentPressed;
    cl->onAccent = l->onAccent; cl->selection.rgb = deep; cl->selection.percent = 11;
}
static inline void brook_hue_derive(unsigned hue, BrookHueTokens* out) {
    if (hue >= BROOK_HUE_COUNT) hue = BROOK_HUE_BLUE;
    brook_hue_derive_from(brook_hue_accent(hue), brook_hue_shaping(hue), out);
}

/* ------------------------------------------------------------------------
 * The painters' palette: every colour the kernel chrome draws, for one
 * (resolved theme, chrome mode, hue). In Glass the chrome sits on the glass
 * tint and the active title top; in the Opaque themes every surface is the
 * flat fill, and the active window is marked by its accent line, rim and
 * full-strength title. Alpha tokens are composited over their surface.
 * ---------------------------------------------------------------------- */
typedef struct BrookThemePalette {
    uint32_t titleActive;
    uint32_t titleInactive;
    uint32_t text;
    uint32_t borderActive;   /* the active window's rim */
    uint32_t borderInactive; /* outer hairline */
    uint32_t closeHover;
    uint32_t closeText;
    uint32_t taskbar;
    uint32_t taskButton;
    uint32_t taskActive;
    uint32_t taskText;
    uint32_t secondaryText;
    uint32_t mutedText;
    uint32_t captionHover;
    uint32_t taskHover;
    uint32_t separator;
    uint32_t minimized;
    uint32_t itemHover;
    uint32_t launcher;
    uint32_t launcherBorder;
    uint32_t gpu3d;
    uint32_t gpu2d;
    uint32_t gpuInactive;
    uint32_t accent;       /* focus stroke, active task, slider fill, graph bars */
    uint32_t halo;         /* outline behind pale text over translucent material */
    uint32_t ok;           /* load and temperature levels */
    uint32_t warm;
    uint32_t hot;
    uint32_t badgeText;    /* build label */
    uint32_t badgeFill;
} BrookThemePalette;

static inline void brook_theme_build_palette(const BrookHueTokens* hue, unsigned resolved, int chromeDark,
                                             BrookThemePalette* p) {
    const BrookChromeTokens* t = &hue->chrome[chromeDark ? 1 : 0];
    const int glass = resolved == BROOK_THEME_GLASS;
    const uint32_t active = glass ? t->titleTop : t->flat, inactive = glass ? t->glass : t->flat;
    const uint32_t bar = glass ? t->glass : t->flat;
    BrookTint idle = t->hover;
    idle.percent = t->hover.percent / 2;
    p->titleActive = active;
    p->titleInactive = inactive;
    p->text = t->ink;
    p->borderActive = brook_tint_over(t->rim, active);
    p->borderInactive = brook_tint_over(t->line, inactive);
    p->closeHover = BROOK_CLOSE_HOVER;
    p->closeText = 0xffffffu;
    p->taskbar = bar;
    p->taskButton = brook_tint_over(idle, bar);
    p->taskActive = brook_tint_over(t->pressed, bar);
    p->taskText = t->ink;
    p->secondaryText = brook_mix(bar, t->ink, t->ink2);
    p->mutedText = brook_mix(bar, t->ink, t->ink3);
    p->captionHover = brook_tint_over(t->hover, active);
    p->taskHover = brook_tint_over(t->hover, bar);
    p->separator = brook_tint_over(t->divider, bar);
    p->minimized = p->mutedText;
    p->itemHover = brook_tint_over(t->hover, bar);
    p->launcher = bar;
    p->launcherBorder = brook_tint_over(t->line, bar);
    p->gpu3d = t->accent;
    p->gpu2d = t->comp;
    p->gpuInactive = p->mutedText;
    p->accent = t->accent;
    p->halo = t->shadow;
    p->ok = t->ok;
    p->warm = t->warm;
    p->hot = t->hot;
    p->badgeText = p->secondaryText;
    p->badgeFill = t->flat;
}
#endif
